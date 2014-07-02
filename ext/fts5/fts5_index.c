/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Low level access to the FTS index stored in the database file. The 
** routines in this file file implement all read and write access to the
** %_data table. Other parts of the system access this functionality via
** the interface defined in fts5Int.h.
*/

#include "fts5Int.h"
#include "fts3_hash.h"

/*
** Overview:
**
** The %_data table contains all the FTS indexes for an FTS5 virtual table.
** As well as the main term index, there may be up to 31 prefix indexes.
** The format is similar to FTS3/4, except that:
**
**   * all segment b-tree leaf data is stored in fixed size page records 
**     (e.g. 1000 bytes). A single doclist may span multiple pages. Care is 
**     taken to ensure it is possible to iterate in either direction through 
**     the entries in a doclist, or to seek to a specific entry within a 
**     doclist, without loading it into memory.
**
**   * large doclists that span many pages have associated "doclist index"
**     records that contain a copy of the first docid on each page spanned by
**     the doclist. This is used to speed up seek operations, and merges of
**     large doclists with very small doclists.
**
**   * extra fields in the "structure record" record the state of ongoing
**     incremental merge operations.
**
*/

#define FTS5_DEFAULT_PAGE_SIZE   1000

#define FTS5_WORK_UNIT      64    /* Number of leaf pages in unit of work */
#define FTS5_MIN_MERGE       4    /* Minimum number of segments to merge */

/*
** Details:
**
** The %_data table managed by this module,
**
**     CREATE TABLE %_data(id INTEGER PRIMARY KEY, block BLOB);
**
** , contains the following 5 types of records. See the comments surrounding
** the FTS5_*_ROWID macros below for a description of how %_data rowids are 
** assigned to each fo them.
**
** 1. Structure Records:
**
**   The set of segments that make up an index - the index structure - are
**   recorded in a single record within the %_data table. The record is a list
**   of SQLite varints. 
**
**   For each level from 0 to nMax:
**
**     + number of input segments in ongoing merge.
**     + total number of segments in level.
**     + for each segment from oldest to newest:
**         + segment id (always > 0)
**         + b-tree height (1 -> root is leaf, 2 -> root is parent of leaf etc.)
**         + first leaf page number (often 1)
**         + final leaf page number
**
** 2. The Averages Record:
**
**   A single record within the %_data table. The data is a list of varints.
**   The first value is the number of rows in the index. Then, for each column
**   from left to right, the total number of tokens in the column for all 
**   rows of the table.
**
** 3. Segment leaves:
**
**   TERM DOCLIST FORMAT:
**
**     Most of each segment leaf is taken up by term/doclist data. The 
**     general format of the term/doclist data is:
**
**         varint : size of first term
**         blob:    first term data
**         doclist: first doclist
**         zero-or-more {
**           varint:  number of bytes in common with previous term
**           varint:  number of bytes of new term data (nNew)
**           blob:    nNew bytes of new term data
**           doclist: next doclist
**         }
**
**     doclist format:
**
**         varint:  first rowid
**         poslist: first poslist
**         zero-or-more {
**           varint:  rowid delta (always > 0)
**           poslist: first poslist
**         }
**         0x00 byte
**
**     poslist format:
**
**         varint: size of poslist in bytes. not including this field.
**         collist: collist for column 0
**         zero-or-more {
**           0x01 byte
**           varint: column number (I)
**           collist: collist for column I
**         }
**
**     collist format:
**
**         varint: first offset + 2
**         zero-or-more {
**           varint: offset delta + 2
**         }
**
**   PAGINATION
**
**     The format described above is only accurate if the entire term/doclist
**     data fits on a single leaf page. If this is not the case, the format
**     is changed in two ways:
**
**       + if the first rowid on a page occurs before the first term, it
**         is stored as a literal value:
**
**             varint:  first rowid
**
**       + the first term on each page is stored in the same way as the
**         very first term of the segment:
**
**             varint : size of first term
**             blob:    first term data
**
**     Each leaf page begins with:
**
**       + 2-byte unsigned containing offset to first rowid (or 0).
**       + 2-byte unsigned containing offset to first term (or 0).
**
**   Followed by term/doclist data.
**
** 4. Segment interior nodes:
**
**   The interior nodes turn the list of leaves into a b+tree. 
**
**   Each interior node begins with a varint - the page number of the left
**   most child node. Following this, for each leaf page except the first,
**   the interior nodes contain:
**
**     a) If the leaf page contains at least one term, then a term-prefix that
**        is greater than all previous terms, and less than or equal to the
**        first term on the leaf page.
**
**     b) If the leaf page no terms, a record indicating how many consecutive
**        leaves contain no terms, and whether or not there is an associated
**        by-rowid index record.
**
**   By definition, there is never more than one type (b) record in a row.
**   Type (b) records only ever appear on height=1 pages - immediate parents
**   of leaves. Only type (a) records are pushed to higher levels.
**
**   Term format:
**
**     * Number of bytes in common with previous term plus 2, as a varint.
**     * Number of bytes of new term data, as a varint.
**     * new term data.
**
**   No-term format:
**
**     * either an 0x00 or 0x01 byte. If the value 0x01 is used, then there 
**       is an associated index-by-rowid record.
**     * the number of zero-term leaves as a varint.
**
** 5. Segment doclist indexes:
**
**   A list of varints - the first docid on each page (starting with the
**   second) of the doclist. First element in the list is a literal docid.
**   Each docid thereafter is a (negative) delta.
*/

/*
** Rowids for the averages and structure records in the %_data table.
*/
#define FTS5_AVERAGES_ROWID     1    /* Rowid used for the averages record */
#define FTS5_STRUCTURE_ROWID(iIdx) (10 + (iIdx))     /* For structure records */

/*
** Macros determining the rowids used by segment nodes. All nodes in all
** segments for all indexes (the regular FTS index and any prefix indexes)
** are stored in the %_data table with large positive rowids.
**
** The %_data table may contain up to (1<<FTS5_SEGMENT_INDEX_BITS) 
** indexes - one regular term index and zero or more prefix indexes.
**
** Each segment in an index has a unique id greater than zero.
**
** Each node in a segment b-tree is assigned a "page number" that is unique
** within nodes of its height within the segment (leaf nodes have a height 
** of 0, parents 1, etc.). Page numbers are allocated sequentially so that
** a nodes page number is always one more than its left sibling.
**
** The rowid for a node is then found using the FTS5_SEGMENT_ROWID() macro
** below. The FTS5_SEGMENT_*_BITS macros define the number of bits used
** to encode the three FTS5_SEGMENT_ROWID() arguments. This module returns
** SQLITE_FULL and fails the current operation if they ever prove too small.
*/
#define FTS5_DATA_IDX_B     5     /* Max of 31 prefix indexes */
#define FTS5_DATA_ID_B     16     /* Max seg id number 65535 */
#define FTS5_DATA_HEIGHT_B  5     /* Max b-tree height of 32 */
#define FTS5_DATA_PAGE_B   31     /* Max page number of 2147483648 */

#define FTS5_SEGMENT_ROWID(idx, segid, height, pgno) (                         \
 ((i64)(idx)    << (FTS5_DATA_ID_B + FTS5_DATA_PAGE_B + FTS5_DATA_HEIGHT_B)) + \
 ((i64)(segid)  << (FTS5_DATA_PAGE_B + FTS5_DATA_HEIGHT_B)) +                  \
 ((i64)(height) << (FTS5_DATA_PAGE_B)) +                                       \
 ((i64)(pgno))                                                                 \
)

#if FTS5_MAX_PREFIX_INDEXES > ((1<<FTS5_DATA_IDX_B)-1) 
# error "FTS5_MAX_PREFIX_INDEXES is too large"
#endif

/*
** The height of segment b-trees is actually limited to one less than 
** (1<<HEIGHT_BITS). This is because the rowid address space for nodes
** with such a height is used by doclist indexes.
*/
#define FTS5_SEGMENT_MAX_HEIGHT ((1 << FTS5_SEGMENT_HEIGHT_BITS)-1)

/*
** The rowid for the doclist index associated with leaf page pgno of segment
** segid in index idx.
*/
#define FTS5_DOCLIST_IDX_ROWID(idx, segid, pgno) \
        FTS5_SEGMENT_ROWID(idx, segid, FTS5_SEGMENT_MAX_HEIGHT, pgno)

#ifdef SQLITE_DEBUG
static int fts5Corrupt() { return SQLITE_CORRUPT_VTAB; }
# define FTS5_CORRUPT fts5Corrupt()
#else
# define FTS5_CORRUPT SQLITE_CORRUPT_VTAB
#endif


typedef struct Fts5BtreeIter Fts5BtreeIter;
typedef struct Fts5BtreeIterLevel Fts5BtreeIterLevel;
typedef struct Fts5ChunkIter Fts5ChunkIter;
typedef struct Fts5Data Fts5Data;
typedef struct Fts5MultiSegIter Fts5MultiSegIter;
typedef struct Fts5NodeIter Fts5NodeIter;
typedef struct Fts5PageWriter Fts5PageWriter;
typedef struct Fts5PendingDoclist Fts5PendingDoclist;
typedef struct Fts5PendingPoslist Fts5PendingPoslist;
typedef struct Fts5PosIter Fts5PosIter;
typedef struct Fts5SegIter Fts5SegIter;
typedef struct Fts5SegWriter Fts5SegWriter;
typedef struct Fts5Structure Fts5Structure;
typedef struct Fts5StructureLevel Fts5StructureLevel;
typedef struct Fts5StructureSegment Fts5StructureSegment;


/*
** One object per %_data table.
*/
struct Fts5Index {
  Fts5Config *pConfig;            /* Virtual table configuration */
  char *zDataTbl;                 /* Name of %_data table */
  int pgsz;                       /* Target page size for this index */
  int nMinMerge;                  /* Minimum input segments in a merge */
  int nWorkUnit;                  /* Leaf pages in a "unit" of work */

  /*
  ** Variables related to the accumulation of tokens and doclists within the
  ** in-memory hash tables before they are flushed to disk.
  */
  Fts3Hash *aHash;                /* One hash for terms, one for each prefix */
  int nMaxPendingData;            /* Max pending data before flush to disk */
  int nPendingData;               /* Current bytes of pending data */
  i64 iWriteRowid;                /* Rowid for current doc being written */

  /* Error state. */
  int rc;                         /* Current error code */

  /* State used by the fts5DataXXX() functions. */
  sqlite3_blob *pReader;          /* RO incr-blob open on %_data table */
  sqlite3_stmt *pWriter;          /* "INSERT ... %_data VALUES(?,?)" */
  sqlite3_stmt *pDeleter;         /* "DELETE FROM %_data ... id>=? AND id<=?" */
};

struct Fts5IndexIter {
  Fts5Index *pIndex;
  Fts5Structure *pStruct;
  Fts5MultiSegIter *pMulti;
  Fts5Buffer poslist;             /* Buffer containing current poslist */
};

/*
** A single record read from the %_data table.
*/
struct Fts5Data {
  u8 *p;                          /* Pointer to buffer containing record */
  int n;                          /* Size of record in bytes */
  int nRef;                       /* Ref count */
};

/*
** Before it is flushed to a level-0 segment, term data is collected in
** the hash tables in the Fts5Index.aHash[] array. Hash table keys are
** terms (or, for prefix indexes, term prefixes) and values are instances
** of type Fts5PendingDoclist.
*/
struct Fts5PendingDoclist {
  u8 *pTerm;                      /* Term for this entry */
  int nTerm;                      /* Bytes of data at pTerm */
  Fts5PendingPoslist *pPoslist;   /* Linked list of position lists */
  int iCol;                       /* Column for last entry in pPending */
  int iPos;                       /* Pos value for last entry in pPending */
  Fts5PendingDoclist *pNext;      /* Used during merge sort */
};
struct Fts5PendingPoslist {
  i64 iRowid;                     /* Rowid for this doclist entry */
  Fts5Buffer buf;                 /* Current doclist contents */
  Fts5PendingPoslist *pNext;      /* Previous poslist for same term */
};

/*
** The contents of the "structure" record for each index are represented
** using an Fts5Structure record in memory. Which uses instances of the 
** other Fts5StructureXXX types as components.
*/
struct Fts5StructureSegment {
  int iSegid;                     /* Segment id */
  int nHeight;                    /* Height of segment b-tree */
  int pgnoFirst;                  /* First leaf page number in segment */
  int pgnoLast;                   /* Last leaf page number in segment */
};
struct Fts5StructureLevel {
  int nMerge;                     /* Number of segments in incr-merge */
  int nSeg;                       /* Total number of segments on level */
  Fts5StructureSegment *aSeg;     /* Array of segments. aSeg[0] is oldest. */
};
struct Fts5Structure {
  u64 nWriteCounter;              /* Total leaves written to level 0 */
  int nLevel;                     /* Number of levels in this index */
  Fts5StructureLevel aLevel[0];   /* Array of nLevel level objects */
};

/*
** An object of type Fts5SegWriter is used to write to segments.
*/
struct Fts5PageWriter {
  int pgno;                       /* Page number for this page */
  Fts5Buffer buf;                 /* Buffer containing page data */
  Fts5Buffer term;                /* Buffer containing previous term on page */
};

struct Fts5SegWriter {
  int iIdx;                       /* Index to write to */
  int iSegid;                     /* Segid to write to */
  int nWriter;                    /* Number of entries in aWriter */
  Fts5PageWriter *aWriter;        /* Array of PageWriter objects */
  i64 iPrevRowid;                 /* Previous docid written to current leaf */
  u8 bFirstRowidInDoclist;        /* True if next rowid is first in doclist */
  u8 bFirstRowidInPage;           /* True if next rowid is first in page */
  int nLeafWritten;               /* Number of leaf pages written */
  int nEmpty;                     /* Number of contiguous term-less nodes */
};

/*
** Object for iterating through the merged results of one or more segments,
** visiting each term/docid pair in the merged data.
**
** nSeg is always a power of two greater than or equal to the number of
** segments that this object is merging data from. Both the aSeg[] and
** aFirst[] arrays are sized at nSeg entries. The aSeg[] array is padded
** with zeroed objects - these are handled as if they were iterators opened
** on empty segments.
**
** The results of comparing segments aSeg[N] and aSeg[N+1], where N is an
** even number, is stored in aFirst[(nSeg+N)/2]. The "result" of the 
** comparison in this context is the index of the iterator that currently
** points to the smaller term/rowid combination. Iterators at EOF are
** considered to be greater than all other iterators.
**
** aFirst[1] contains the index in aSeg[] of the iterator that points to
** the smallest key overall. aFirst[0] is unused. 
*/
struct Fts5MultiSegIter {
  int nSeg;                       /* Size of aSeg[] array */
  Fts5SegIter *aSeg;              /* Array of segment iterators */
  u16 *aFirst;                    /* Current merge state (see above) */
};

/*
** Object for iterating through a single segment, visiting each term/docid
** pair in the segment.
**
** pSeg:
**   The segment to iterate through.
**
** iLeafPgno:
**   Current leaf page number within segment.
**
** iLeafOffset:
**   Byte offset within the current leaf that is one byte past the end of the
**   rowid field of the current entry. Usually this is the size field of the
**   position list data. The exception is if the rowid for the current entry 
**   is the last thing on the leaf page.
**
** pLeaf:
**   Buffer containing current leaf page data. Set to NULL at EOF.
**
** iTermLeafPgno, iTermLeafOffset:
**   Leaf page number containing the last term read from the segment. And
**   the offset immediately following the term data.
**
** bOneTerm:
**   If true, set the iterator to point to EOF after the current doclist has
**   been exhausted. Do not proceed to the next term in the segment.
*/
struct Fts5SegIter {
  Fts5StructureSegment *pSeg;     /* Segment to iterate through */
  int iIdx;                       /* Byte offset within current leaf */
  int bOneTerm;                   /* If true, iterate through single doclist */
  int iLeafPgno;                  /* Current leaf page number */
  Fts5Data *pLeaf;                /* Current leaf data */
  int iLeafOffset;                /* Byte offset within current leaf */

  int iTermLeafPgno;
  int iTermLeafOffset;

  /* Variables populated based on current entry. */
  Fts5Buffer term;                /* Current term */
  i64 iRowid;                     /* Current rowid */
};

/*
** Object for iterating through paginated data.
*/
struct Fts5ChunkIter {
  Fts5Data *pLeaf;                /* Current leaf data. NULL -> EOF. */
  i64 iLeafRowid;                 /* Absolute rowid of current leaf */
  int nRem;                       /* Remaining bytes of data to read */

  /* Output parameters */
  u8 *p;                          /* Pointer to chunk of data */
  int n;                          /* Size of buffer p in bytes */
};

/*
** Object for iterating through a single position list.
*/
struct Fts5PosIter {
  Fts5ChunkIter chunk;            /* Current chunk of data */
  int iOff;                       /* Offset within chunk data */

  int iCol;
  int iPos;
};

/*
** Object for iterating through the conents of a single internal node in 
** memory.
*/
struct Fts5NodeIter {
  /* Internal. Set and managed by fts5NodeIterXXX() functions. Except, 
  ** the EOF test for the iterator is (Fts5NodeIter.aData==0).  */
  const u8 *aData;
  int nData;
  int iOff;

  /* Output variables */
  Fts5Buffer term;
  int nEmpty;
  int iChild;
};

/*
** An Fts5BtreeIter object is used to iterate through all entries in the
** b-tree hierarchy belonging to a single fts5 segment. In this case the
** "b-tree hierarchy" is all b-tree nodes except leaves. Each entry in the
** b-tree hierarchy consists of the following:
**
**   iLeaf:  The page number of the leaf page the entry points to.
**
**   term:   A split-key that all terms on leaf page $leaf must be greater
**           than or equal to. The "term" associated with the first b-tree
**           hierarchy entry (the one that points to leaf page 1) is always 
**           an empty string.
**
**   nEmpty: The number of empty (termless) leaf pages that immediately
**           following iLeaf.
**
** The Fts5BtreeIter object is only used as part of the integrity-check code.
*/
struct Fts5BtreeIterLevel {
  Fts5NodeIter s;                 /* Iterator for the current node */
  Fts5Data *pData;                /* Data for the current node */
};
struct Fts5BtreeIter {
  Fts5Index *p;                   /* FTS5 backend object */
  Fts5StructureSegment *pSeg;     /* Iterate through this segment's b-tree */
  int iIdx;                       /* Index pSeg belongs to */
  int nLvl;                       /* Size of aLvl[] array */
  Fts5BtreeIterLevel *aLvl;       /* Level for each tier of b-tree */

  /* Output variables */
  Fts5Buffer term;                /* Current term */
  int iLeaf;                      /* Leaf containing terms >= current term */
  int nEmpty;                     /* Number of "empty" leaves following iLeaf */
  int bEof;                       /* Set to true at EOF */
};

static void fts5PutU16(u8 *aOut, u16 iVal){
  aOut[0] = (iVal>>8);
  aOut[1] = (iVal&0xFF);
}

static u16 fts5GetU16(const u8 *aIn){
  return ((u16)aIn[0] << 8) + aIn[1];
}

/*
** Allocate and return a buffer at least nByte bytes in size.
**
** If an OOM error is encountered, return NULL and set the error code in
** the Fts5Index handle passed as the first argument.
*/
static void *fts5IdxMalloc(Fts5Index *p, int nByte){
  void *pRet;
  assert( p->rc==SQLITE_OK );
  pRet = sqlite3_malloc(nByte);
  if( pRet==0 ){
    p->rc = SQLITE_NOMEM;
  }else{
    memset(pRet, 0, nByte);
  }
  return pRet;
}


/*
** Compare the contents of the pLeft buffer with the pRight/nRight blob.
**
** Return -ve if pLeft is smaller than pRight, 0 if they are equal or
** +ve if pRight is smaller than pLeft. In other words:
**
**     res = *pLeft - *pRight
*/
static int fts5BufferCompareBlob(
  Fts5Buffer *pLeft,              /* Left hand side of comparison */
  const u8 *pRight, int nRight    /* Right hand side of comparison */
){
  int nCmp = MIN(pLeft->n, nRight);
  int res = memcmp(pLeft->p, pRight, nCmp);
  return (res==0 ? (pLeft->n - nRight) : res);
}

/*
** Compare the contents of the two buffers using memcmp(). If one buffer
** is a prefix of the other, it is considered the lesser.
**
** Return -ve if pLeft is smaller than pRight, 0 if they are equal or
** +ve if pRight is smaller than pLeft. In other words:
**
**     res = *pLeft - *pRight
*/
static int fts5BufferCompare(Fts5Buffer *pLeft, Fts5Buffer *pRight){
  int nCmp = MIN(pLeft->n, pRight->n);
  int res = memcmp(pLeft->p, pRight->p, nCmp);
  return (res==0 ? (pLeft->n - pRight->n) : res);
}


/*
** Close the read-only blob handle, if it is open.
*/
static void fts5CloseReader(Fts5Index *p){
  if( p->pReader ){
    sqlite3_blob_close(p->pReader);
    p->pReader = 0;
  }
}

static Fts5Data *fts5DataReadOrBuffer(
  Fts5Index *p, 
  Fts5Buffer *pBuf, 
  i64 iRowid
){
  Fts5Data *pRet = 0;
  if( p->rc==SQLITE_OK ){
    int rc;

    /* If the blob handle is not yet open, open and seek it. Otherwise, use
    ** the blob_reopen() API to reseek the existing blob handle.  */
    if( p->pReader==0 ){
      Fts5Config *pConfig = p->pConfig;
      rc = sqlite3_blob_open(pConfig->db, 
          pConfig->zDb, p->zDataTbl, "block", iRowid, 0, &p->pReader
      );
    }else{
      rc = sqlite3_blob_reopen(p->pReader, iRowid);
    }

    if( rc==SQLITE_OK ){
      int nByte = sqlite3_blob_bytes(p->pReader);
      if( pBuf ){
        fts5BufferZero(pBuf);
        fts5BufferGrow(&rc, pBuf, nByte);
        rc = sqlite3_blob_read(p->pReader, pBuf->p, nByte, 0);
        if( rc==SQLITE_OK ) pBuf->n = nByte;
      }else{
        pRet = (Fts5Data*)fts5IdxMalloc(p, sizeof(Fts5Data) + nByte);
        if( !pRet ) return 0;

        pRet->n = nByte;
        pRet->p = (u8*)&pRet[1];
        pRet->nRef = 1;
        rc = sqlite3_blob_read(p->pReader, pRet->p, nByte, 0);
        if( rc!=SQLITE_OK ){
          sqlite3_free(pRet);
          pRet = 0;
        }
      }
    }
    p->rc = rc;
  }

  return pRet;
}

/*
** Retrieve a record from the %_data table.
**
** If an error occurs, NULL is returned and an error left in the 
** Fts5Index object.
*/
static Fts5Data *fts5DataRead(Fts5Index *p, i64 iRowid){
  Fts5Data *pRet = fts5DataReadOrBuffer(p, 0, iRowid);
  assert( (pRet==0)==(p->rc!=SQLITE_OK) );
  return pRet;
}

/*
** Read a record from the %_data table into the buffer supplied as the
** second argument.
**
** If an error occurs, an error is left in the Fts5Index object. If an
** error has already occurred when this function is called, it is a 
** no-op.
*/
static void fts5DataBuffer(Fts5Index *p, Fts5Buffer *pBuf, i64 iRowid){
  (void)fts5DataReadOrBuffer(p, pBuf, iRowid);
}

/*
** Release a reference to data record returned by an earlier call to
** fts5DataRead().
*/
static void fts5DataRelease(Fts5Data *pData){
  if( pData ){
    pData->nRef--;
    if( pData->nRef==0 ) sqlite3_free(pData);
  }
}

static void fts5DataReference(Fts5Data *pData){
  pData->nRef++;
}

/*
** INSERT OR REPLACE a record into the %_data table.
*/
static void fts5DataWrite(Fts5Index *p, i64 iRowid, u8 *pData, int nData){
  if( p->rc!=SQLITE_OK ) return;

  if( p->pWriter==0 ){
    int rc;
    Fts5Config *pConfig = p->pConfig;
    char *zSql = sqlite3_mprintf(
        "REPLACE INTO '%q'.%Q(id, block) VALUES(?,?)", pConfig->zDb, p->zDataTbl
    );
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_prepare_v2(pConfig->db, zSql, -1, &p->pWriter, 0);
      sqlite3_free(zSql);
    }
    if( rc!=SQLITE_OK ){
      p->rc = rc;
      return;
    }
  }

  sqlite3_bind_int64(p->pWriter, 1, iRowid);
  sqlite3_bind_blob(p->pWriter, 2, pData, nData, SQLITE_STATIC);
  sqlite3_step(p->pWriter);
  p->rc = sqlite3_reset(p->pWriter);
}

/*
** Execute the following SQL:
**
**     DELETE FROM %_data WHERE id BETWEEN $iFirst AND $iLast
*/
static void fts5DataDelete(Fts5Index *p, i64 iFirst, i64 iLast){
  if( p->rc!=SQLITE_OK ) return;

  if( p->pDeleter==0 ){
    int rc;
    Fts5Config *pConfig = p->pConfig;
    char *zSql = sqlite3_mprintf(
        "DELETE FROM '%q'.%Q WHERE id>=? AND id<=?", pConfig->zDb, p->zDataTbl
    );
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_prepare_v2(pConfig->db, zSql, -1, &p->pDeleter, 0);
      sqlite3_free(zSql);
    }
    if( rc!=SQLITE_OK ){
      p->rc = rc;
      return;
    }
  }

  sqlite3_bind_int64(p->pDeleter, 1, iFirst);
  sqlite3_bind_int64(p->pDeleter, 2, iLast);
  sqlite3_step(p->pDeleter);
  p->rc = sqlite3_reset(p->pDeleter);
}

/*
** Close the sqlite3_blob handle used to read records from the %_data table.
** And discard any cached reads. This function is called at the end of
** a read transaction or when any sub-transaction is rolled back.
*/
static void fts5DataReset(Fts5Index *p){
  if( p->pReader ){
    sqlite3_blob_close(p->pReader);
    p->pReader = 0;
  }
}

/*
** Remove all records associated with segment iSegid in index iIdx.
*/
static void fts5DataRemoveSegment(Fts5Index *p, int iIdx, int iSegid){
  i64 iFirst = FTS5_SEGMENT_ROWID(iIdx, iSegid, 0, 0);
  i64 iLast = FTS5_SEGMENT_ROWID(iIdx, iSegid+1, 0, 0)-1;
  fts5DataDelete(p, iFirst, iLast);
}

/*
** Deserialize and return the structure record currently stored in serialized
** form within buffer pData/nData.
**
** The Fts5Structure.aLevel[] and each Fts5StructureLevel.aSeg[] array
** are over-allocated by one slot. This allows the structure contents
** to be more easily edited.
**
** If an error occurs, *ppOut is set to NULL and an SQLite error code
** returned. Otherwise, *ppOut is set to point to the new object and
** SQLITE_OK returned.
*/
static int fts5StructureDecode(
  const u8 *pData,                /* Buffer containing serialized structure */
  int nData,                      /* Size of buffer pData in bytes */
  Fts5Structure **ppOut           /* OUT: Deserialized object */
){
  int rc = SQLITE_OK;
  int i = 0;
  int iLvl;
  int nLevel = 0;
  int nSegment = 0;
  int nByte;                      /* Bytes of space to allocate */
  Fts5Structure *pRet = 0;

  /* Read the total number of levels and segments from the start of the
  ** structure record. Use these values to allocate space for the deserialized
  ** version of the record. */
  i = getVarint32(&pData[i], nLevel);
  i += getVarint32(&pData[i], nSegment);
  nByte = (
      sizeof(Fts5Structure) + 
      sizeof(Fts5StructureLevel) * (nLevel+1) +
      sizeof(Fts5StructureSegment) * (nSegment+nLevel+1)
  );
  pRet = (Fts5Structure*)sqlite3_malloc(nByte);

  if( pRet ){
    u8 *pSpace = (u8*)&pRet->aLevel[nLevel+1];
    memset(pRet, 0, nByte);
    pRet->nLevel = nLevel;
    i += sqlite3GetVarint(&pData[i], &pRet->nWriteCounter);
    for(iLvl=0; iLvl<nLevel; iLvl++){
      Fts5StructureLevel *pLvl = &pRet->aLevel[iLvl];
      int nTotal;
      int iSeg;

      i += getVarint32(&pData[i], pLvl->nMerge);
      i += getVarint32(&pData[i], nTotal);
      assert( nTotal>=pLvl->nMerge );
      pLvl->nSeg = nTotal;
      pLvl->aSeg = (Fts5StructureSegment*)pSpace;
      pSpace += ((nTotal+1) * sizeof(Fts5StructureSegment));

      for(iSeg=0; iSeg<nTotal; iSeg++){
        i += getVarint32(&pData[i], pLvl->aSeg[iSeg].iSegid);
        i += getVarint32(&pData[i], pLvl->aSeg[iSeg].nHeight);
        i += getVarint32(&pData[i], pLvl->aSeg[iSeg].pgnoFirst);
        i += getVarint32(&pData[i], pLvl->aSeg[iSeg].pgnoLast);
      }
    }
    pRet->aLevel[nLevel].aSeg = (Fts5StructureSegment*)pSpace;
  }else{
    rc = SQLITE_NOMEM;
  }

  *ppOut = pRet;
  return rc;
}

/*
** Read, deserialize and return the structure record for index iIdx.
**
** The Fts5Structure.aLevel[] and each Fts5StructureLevel.aSeg[] array
** are over-allocated as described for function fts5StructureDecode() 
** above.
**
** If an error occurs, NULL is returned and an error code left in the
** Fts5Index handle. If an error has already occurred when this function
** is called, it is a no-op.
*/
static Fts5Structure *fts5StructureRead(Fts5Index *p, int iIdx){
  Fts5Config *pConfig = p->pConfig;
  Fts5Structure *pRet = 0;        /* Object to return */
  Fts5Data *pData;                /* %_data entry containing structure record */

  assert( iIdx<=pConfig->nPrefix );
  pData = fts5DataRead(p, FTS5_STRUCTURE_ROWID(iIdx));
  if( !pData ) return 0;
  p->rc = fts5StructureDecode(pData->p, pData->n, &pRet);

  fts5DataRelease(pData);
  return pRet;
}

/*
** Release a reference to an Fts5Structure object returned by an earlier 
** call to fts5StructureRead() or fts5StructureDecode().
*/
static void fts5StructureRelease(Fts5Structure *pStruct){
  sqlite3_free(pStruct);
}

/*
** Return the total number of segments in index structure pStruct.
*/
static int fts5StructureCountSegments(Fts5Structure *pStruct){
  int nSegment = 0;               /* Total number of segments */
  int iLvl;                       /* Used to iterate through levels */

  for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
    nSegment += pStruct->aLevel[iLvl].nSeg;
  }

  return nSegment;
}

/*
** Serialize and store the "structure" record for index iIdx.
**
** If an error occurs, leave an error code in the Fts5Index object. If an
** error has already occurred, this function is a no-op.
*/
static void fts5StructureWrite(Fts5Index *p, int iIdx, Fts5Structure *pStruct){
  int nSegment;                   /* Total number of segments */
  Fts5Buffer buf;                 /* Buffer to serialize record into */
  int iLvl;                       /* Used to iterate through levels */

  nSegment = fts5StructureCountSegments(pStruct);
  memset(&buf, 0, sizeof(Fts5Buffer));
  fts5BufferAppendVarint(&p->rc, &buf, pStruct->nLevel);
  fts5BufferAppendVarint(&p->rc, &buf, nSegment);
  fts5BufferAppendVarint(&p->rc, &buf, (i64)pStruct->nWriteCounter);

  for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
    int iSeg;                     /* Used to iterate through segments */
    Fts5StructureLevel *pLvl = &pStruct->aLevel[iLvl];
    fts5BufferAppendVarint(&p->rc, &buf, pLvl->nMerge);
    fts5BufferAppendVarint(&p->rc, &buf, pLvl->nSeg);

    for(iSeg=0; iSeg<pLvl->nSeg; iSeg++){
      fts5BufferAppendVarint(&p->rc, &buf, pLvl->aSeg[iSeg].iSegid);
      fts5BufferAppendVarint(&p->rc, &buf, pLvl->aSeg[iSeg].nHeight);
      fts5BufferAppendVarint(&p->rc, &buf, pLvl->aSeg[iSeg].pgnoFirst);
      fts5BufferAppendVarint(&p->rc, &buf, pLvl->aSeg[iSeg].pgnoLast);
    }
  }

  fts5DataWrite(p, FTS5_STRUCTURE_ROWID(iIdx), buf.p, buf.n);
  fts5BufferFree(&buf);
}


/*
** If the pIter->iOff offset currently points to an entry indicating one
** or more term-less nodes, advance past it and set pIter->nEmpty to
** the number of empty child nodes.
*/
static void fts5NodeIterGobbleNEmpty(Fts5NodeIter *pIter){
  if( pIter->iOff<pIter->nData && 0==(pIter->aData[pIter->iOff] & 0xfe) ){
    pIter->iOff++;
    pIter->iOff += getVarint32(&pIter->aData[pIter->iOff], pIter->nEmpty);
  }else{
    pIter->nEmpty = 0;
  }
}

/*
** Advance to the next entry within the node.
*/
static void fts5NodeIterNext(int *pRc, Fts5NodeIter *pIter){
  if( pIter->iOff>=pIter->nData ){
    pIter->aData = 0;
    pIter->iChild += pIter->nEmpty;
  }else{
    int nPre, nNew;
    pIter->iOff += getVarint32(&pIter->aData[pIter->iOff], nPre);
    pIter->iOff += getVarint32(&pIter->aData[pIter->iOff], nNew);
    pIter->term.n = nPre-2;
    fts5BufferAppendBlob(pRc, &pIter->term, nNew, pIter->aData+pIter->iOff);
    pIter->iOff += nNew;
    pIter->iChild += (1 + pIter->nEmpty);
    fts5NodeIterGobbleNEmpty(pIter);
    if( *pRc ) pIter->aData = 0;
  }
}


/*
** Initialize the iterator object pIter to iterate through the internal
** segment node in pData.
*/
static void fts5NodeIterInit(const u8 *aData, int nData, Fts5NodeIter *pIter){
  memset(pIter, 0, sizeof(*pIter));
  pIter->aData = aData;
  pIter->nData = nData;
  pIter->iOff = getVarint32(aData, pIter->iChild);
  fts5NodeIterGobbleNEmpty(pIter);
}

/*
** Free any memory allocated by the iterator object.
*/
static void fts5NodeIterFree(Fts5NodeIter *pIter){
  fts5BufferFree(&pIter->term);
}

/*
** Load the next leaf page into the segment iterator.
*/
static void fts5SegIterNextPage(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter              /* Iterator to advance to next page */
){
  Fts5StructureSegment *pSeg = pIter->pSeg;
  if( pIter->pLeaf ) fts5DataRelease(pIter->pLeaf);
  pIter->iLeafPgno++;
  if( pIter->iLeafPgno<=pSeg->pgnoLast ){
    pIter->pLeaf = fts5DataRead(p, 
        FTS5_SEGMENT_ROWID(pIter->iIdx, pSeg->iSegid, 0, pIter->iLeafPgno)
    );
  }else{
    pIter->pLeaf = 0;
  }
}

/*
** Leave pIter->iLeafOffset as the offset to the size field of the first
** position list. The position list belonging to document pIter->iRowid.
*/
static void fts5SegIterLoadTerm(Fts5Index *p, Fts5SegIter *pIter, int nKeep){
  u8 *a = pIter->pLeaf->p;        /* Buffer to read data from */
  int iOff = pIter->iLeafOffset;  /* Offset to read at */
  int nNew;                       /* Bytes of new data */

  iOff += getVarint32(&a[iOff], nNew);
  pIter->term.n = nKeep;
  fts5BufferAppendBlob(&p->rc, &pIter->term, nNew, &a[iOff]);
  iOff += nNew;
  pIter->iTermLeafOffset = iOff;
  pIter->iTermLeafPgno = pIter->iLeafPgno;
  if( iOff>=pIter->pLeaf->n ){
    fts5SegIterNextPage(p, pIter);
    if( pIter->pLeaf==0 ){
      if( p->rc==SQLITE_OK ) p->rc = FTS5_CORRUPT;
      return;
    }
    iOff = 4;
    a = pIter->pLeaf->p;
  }
  iOff += sqlite3GetVarint(&a[iOff], (u64*)&pIter->iRowid);
  pIter->iLeafOffset = iOff;
}

/*
** Initialize the iterator object pIter to iterate through the entries in
** segment pSeg within index iIdx. The iterator is left pointing to the 
** first entry when this function returns.
**
** If an error occurs, Fts5Index.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void fts5SegIterInit(
  Fts5Index *p,          
  int iIdx,                       /* Config.aHash[] index of FTS index */
  Fts5StructureSegment *pSeg,     /* Description of segment */
  Fts5SegIter *pIter              /* Object to populate */
){

  if( p->rc==SQLITE_OK ){
    memset(pIter, 0, sizeof(*pIter));
    pIter->pSeg = pSeg;
    pIter->iIdx = iIdx;
    pIter->iLeafPgno = pSeg->pgnoFirst-1;
    fts5SegIterNextPage(p, pIter);
  }

  if( p->rc==SQLITE_OK ){
    u8 *a = pIter->pLeaf->p;
    pIter->iLeafOffset = fts5GetU16(&a[2]);
    fts5SegIterLoadTerm(p, pIter, 0);
  }
}

/*
** Initialize the object pIter to point to term pTerm/nTerm within segment
** pSeg, index iIdx. If there is no such term in the index, the iterator
** is set to EOF.
**
** If an error occurs, Fts5Index.rc is set to an appropriate error code. If 
** an error has already occurred when this function is called, it is a no-op.
*/
static void fts5SegIterSeekInit(
  Fts5Index *p,                   /* FTS5 backend */
  int iIdx,                       /* Config.aHash[] index of FTS index */
  const u8 *pTerm, int nTerm,     /* Term to seek to */
  Fts5StructureSegment *pSeg,     /* Description of segment */
  Fts5SegIter *pIter              /* Object to populate */
){
  int iPg = 1;
  int h;

  assert( pTerm && nTerm );
  memset(pIter, 0, sizeof(*pIter));
  pIter->pSeg = pSeg;
  pIter->iIdx = iIdx;
  pIter->bOneTerm = 1;

  for(h=pSeg->nHeight-1; h>0; h--){
    Fts5NodeIter node;              /* For iterating through internal nodes */
    i64 iRowid = FTS5_SEGMENT_ROWID(iIdx, pSeg->iSegid, h, iPg);
    Fts5Data *pNode = fts5DataRead(p, iRowid);
    if( pNode==0 ) break;

    fts5NodeIterInit(pNode->p, pNode->n, &node);
    assert( node.term.n==0 );

    iPg = node.iChild;
    for(fts5NodeIterNext(&p->rc, &node);
        node.aData && fts5BufferCompareBlob(&node.term, pTerm, nTerm)<=0;
        fts5NodeIterNext(&p->rc, &node)
    ){
      iPg = node.iChild;
    }
    fts5NodeIterFree(&node);
    fts5DataRelease(pNode);
  }

  if( iPg>=pSeg->pgnoFirst ){
    int res;
    pIter->iLeafPgno = iPg - 1;
    fts5SegIterNextPage(p, pIter);
    if( pIter->pLeaf ){
      u8 *a = pIter->pLeaf->p;
      int n = pIter->pLeaf->n;

      pIter->iLeafOffset = fts5GetU16(&a[2]);
      fts5SegIterLoadTerm(p, pIter, 0);

      while( (res = fts5BufferCompareBlob(&pIter->term, pTerm, nTerm)) ){
        if( res<0 && pIter->iLeafPgno==iPg ){
          int iOff = pIter->iLeafOffset;
          while( iOff<n ){
            int nPos;
            iOff += getVarint32(&a[iOff], nPos);
            iOff += nPos;
            if( iOff<n ){
              u64 rowid;
              iOff += getVarint(&a[iOff], &rowid);
              if( rowid==0 ) break;
            }
          }

          /* If the iterator is not yet at the end of the next page, load
          ** the next term and jump to the next iteration of the while()
          ** loop.  */
          if( iOff<n ){
            int nKeep;
            pIter->iLeafOffset = iOff + getVarint32(&a[iOff], nKeep);
            fts5SegIterLoadTerm(p, pIter, nKeep);
            continue;
          }
        }

        /* No matching term on this page. Set the iterator to EOF. */
        fts5DataRelease(pIter->pLeaf);
        pIter->pLeaf = 0;
        break;
      }
    }
  }
}

/*
** Advance iterator pIter to the next entry. 
**
** If an error occurs, Fts5Index.rc is set to an appropriate error code. It 
** is not considered an error if the iterator reaches EOF. If an error has 
** already occurred when this function is called, it is a no-op.
*/
static void fts5SegIterNext(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pIter              /* Iterator to advance */
){
  if( p->rc==SQLITE_OK ){
    Fts5Data *pLeaf = pIter->pLeaf;
    int iOff;
    int bNewTerm = 0;
    int nKeep = 0;

    /* Search for the end of the position list within the current page. */
    u8 *a = pLeaf->p;
    int n = pLeaf->n;

    iOff = pIter->iLeafOffset;
    if( iOff<=n ){
      int nPoslist;
      iOff += getVarint32(&a[iOff], nPoslist);
      iOff += nPoslist;
    }

    if( iOff<n ){
      /* The next entry is on the current page */
      u64 iDelta;
      iOff += sqlite3GetVarint(&a[iOff], &iDelta);
      pIter->iLeafOffset = iOff;
      if( iDelta==0 ){
        bNewTerm = 1;
        if( iOff>=n ){
          fts5SegIterNextPage(p, pIter);
          pIter->iLeafOffset = 4;
        }else if( iOff!=fts5GetU16(&a[2]) ){
          pIter->iLeafOffset += getVarint32(&a[iOff], nKeep);
        }
      }else{
        pIter->iRowid -= iDelta;
      }
    }else{
      iOff = 0;
      /* Next entry is not on the current page */
      while( iOff==0 ){
        fts5SegIterNextPage(p, pIter);
        pLeaf = pIter->pLeaf;
        if( pLeaf==0 ) break;
        if( (iOff = fts5GetU16(&pLeaf->p[0])) ){
          iOff += sqlite3GetVarint(&pLeaf->p[iOff], (u64*)&pIter->iRowid);
          pIter->iLeafOffset = iOff;
        }
        else if( (iOff = fts5GetU16(&pLeaf->p[2])) ){
          pIter->iLeafOffset = iOff;
          bNewTerm = 1;
        }
      }
    }

    /* Check if the iterator is now at EOF. If so, return early. */
    if( pIter->pLeaf && bNewTerm ){
      if( pIter->bOneTerm ){
        fts5DataRelease(pIter->pLeaf);
        pIter->pLeaf = 0;
      }else{
        fts5SegIterLoadTerm(p, pIter, nKeep);
      }
    }
  }
}

/*
** Zero the iterator passed as the only argument.
*/
static void fts5SegIterClear(Fts5SegIter *pIter){
  fts5BufferFree(&pIter->term);
  fts5DataRelease(pIter->pLeaf);
  memset(pIter, 0, sizeof(Fts5SegIter));
}

/*
** Do the comparison necessary to populate pIter->aFirst[iOut].
**
** If the returned value is non-zero, then it is the index of an entry
** in the pIter->aSeg[] array that is (a) not at EOF, and (b) pointing
** to a key that is a duplicate of another, higher priority, 
** segment-iterator in the pSeg->aSeg[] array.
*/
static int fts5MultiIterDoCompare(Fts5MultiSegIter *pIter, int iOut){
  int i1;                         /* Index of left-hand Fts5SegIter */
  int i2;                         /* Index of right-hand Fts5SegIter */
  int iRes;
  Fts5SegIter *p1;                /* Left-hand Fts5SegIter */
  Fts5SegIter *p2;                /* Right-hand Fts5SegIter */

  assert( iOut<pIter->nSeg && iOut>0 );

  if( iOut>=(pIter->nSeg/2) ){
    i1 = (iOut - pIter->nSeg/2) * 2;
    i2 = i1 + 1;
  }else{
    i1 = pIter->aFirst[iOut*2];
    i2 = pIter->aFirst[iOut*2+1];
  }
  p1 = &pIter->aSeg[i1];
  p2 = &pIter->aSeg[i2];

  if( p1->pLeaf==0 ){           /* If p1 is at EOF */
    iRes = i2;
  }else if( p2->pLeaf==0 ){     /* If p2 is at EOF */
    iRes = i1;
  }else{
    int res = fts5BufferCompare(&p1->term, &p2->term);
    if( res==0 ){
      assert( i2>i1 );
      assert( i2!=0 );
      if( p1->iRowid==p2->iRowid ) return i2;
      res = (p1->iRowid > p2->iRowid) ? -1 : +1;
    }
    assert( res!=0 );
    if( res<0 ){
      iRes = i1;
    }else{
      iRes = i2;
    }
  }

  pIter->aFirst[iOut] = iRes;
  return 0;
}

/*
** Free the iterator object passed as the second argument.
*/
static void fts5MultiIterFree(Fts5Index *p, Fts5MultiSegIter *pIter){
  if( pIter ){
    int i;
    for(i=0; i<pIter->nSeg; i++){
      fts5SegIterClear(&pIter->aSeg[i]);
    }
    sqlite3_free(pIter);
  }
}

static void fts5MultiIterAdvanced(
  Fts5Index *p,                   /* FTS5 backend to iterate within */
  Fts5MultiSegIter *pIter,        /* Iterator to update aFirst[] array for */
  int iChanged,                   /* Index of sub-iterator just advanced */
  int iMinset                     /* Minimum entry in aFirst[] to set */
){
  int i;
  for(i=(pIter->nSeg+iChanged)/2; i>=iMinset && p->rc==SQLITE_OK; i=i/2){
    int iEq;
    if( (iEq = fts5MultiIterDoCompare(pIter, i)) ){
      fts5SegIterNext(p, &pIter->aSeg[iEq]);
      i = pIter->nSeg + iEq;
    }
  }
}

/*
** Move the iterator to the next entry. 
**
** If an error occurs, an error code is left in Fts5Index.rc. It is not 
** considered an error if the iterator reaches EOF, or if it is already at 
** EOF when this function is called.
*/
static void fts5MultiIterNext(Fts5Index *p, Fts5MultiSegIter *pIter){
  if( p->rc==SQLITE_OK ){
    int iFirst = pIter->aFirst[1];
    fts5SegIterNext(p, &pIter->aSeg[iFirst]);
    fts5MultiIterAdvanced(p, pIter, iFirst, 1);
  }
}

/*
** Allocate a new Fts5MultiSegIter object.
**
** The new object will be used to iterate through data in structure pStruct.
** If iLevel is -ve, then all data in all segments is merged. Or, if iLevel
** is zero or greater, data from the first nSegment segments on level iLevel
** is merged.
**
** The iterator initially points to the first term/rowid entry in the 
** iterated data.
*/
static void fts5MultiIterNew(
  Fts5Index *p,                   /* FTS5 backend to iterate within */
  Fts5Structure *pStruct,         /* Structure of specific index */
  int iIdx,                       /* Config.aHash[] index of FTS index */
  const u8 *pTerm, int nTerm,     /* Term to seek to (or NULL/0) */
  int iLevel,                     /* Level to iterate (-1 for all) */
  int nSegment,                   /* Number of segments to merge (iLevel>=0) */
  Fts5MultiSegIter **ppOut        /* New object */
){
  int nSeg;                       /* Number of segments merged */
  int nSlot;                      /* Power of two >= nSeg */
  int iIter = 0;                  /* */
  int iSeg;                       /* Used to iterate through segments */
  Fts5StructureLevel *pLvl;
  Fts5MultiSegIter *pNew;

  assert( (pTerm==0 && nTerm==0) || iLevel<0 );

  /* Allocate space for the new multi-seg-iterator. */
  if( iLevel<0 ){
    nSeg = fts5StructureCountSegments(pStruct);
  }else{
    nSeg = MIN(pStruct->aLevel[iLevel].nSeg, nSegment);
  }
  for(nSlot=2; nSlot<nSeg; nSlot=nSlot*2);
  *ppOut = pNew = fts5IdxMalloc(p, 
      sizeof(Fts5MultiSegIter) +          /* pNew */
      sizeof(Fts5SegIter) * nSlot +       /* pNew->aSeg[] */
      sizeof(u16) * nSlot                 /* pNew->aFirst[] */
  );
  if( pNew==0 ) return;
  pNew->nSeg = nSlot;
  pNew->aSeg = (Fts5SegIter*)&pNew[1];
  pNew->aFirst = (u16*)&pNew->aSeg[nSlot];

  /* Initialize each of the component segment iterators. */
  if( iLevel<0 ){
    Fts5StructureLevel *pEnd = &pStruct->aLevel[pStruct->nLevel];
    for(pLvl=&pStruct->aLevel[0]; pLvl<pEnd; pLvl++){
      for(iSeg=pLvl->nSeg-1; iSeg>=0; iSeg--){
        Fts5SegIter *pIter = &pNew->aSeg[iIter++];
        if( pTerm==0 ){
          fts5SegIterInit(p, iIdx, &pLvl->aSeg[iSeg], pIter);
        }else{
          fts5SegIterSeekInit(p, iIdx, pTerm, nTerm, &pLvl->aSeg[iSeg], pIter);
        }
      }
    }
  }else{
    pLvl = &pStruct->aLevel[iLevel];
    for(iSeg=nSeg-1; iSeg>=0; iSeg--){
      fts5SegIterInit(p, iIdx, &pLvl->aSeg[iSeg], &pNew->aSeg[iIter++]);
    }
  }
  assert( iIter==nSeg );

  /* If the above was successful, each component iterators now points 
  ** to the first entry in its segment. In this case initialize the 
  ** aFirst[] array. Or, if an error has occurred, free the iterator
  ** object and set the output variable to NULL.  */
  if( p->rc==SQLITE_OK ){
    for(iIter=nSlot-1; iIter>0; iIter--){
      int iEq;
      if( (iEq = fts5MultiIterDoCompare(pNew, iIter)) ){
        fts5SegIterNext(p, &pNew->aSeg[iEq]);
        fts5MultiIterAdvanced(p, pNew, iEq, iIter);
      }
    }
  }else{
    fts5MultiIterFree(p, pNew);
    *ppOut = 0;
  }
}

/*
** Return true if the iterator is at EOF or if an error has occurred. 
** False otherwise.
*/
static int fts5MultiIterEof(Fts5Index *p, Fts5MultiSegIter *pIter){
  return (p->rc || pIter->aSeg[ pIter->aFirst[1] ].pLeaf==0);
}

/*
** Return the rowid of the entry that the iterator currently points
** to. If the iterator points to EOF when this function is called the
** results are undefined.
*/
static i64 fts5MultiIterRowid(Fts5MultiSegIter *pIter){
  return pIter->aSeg[ pIter->aFirst[1] ].iRowid;
}

/*
** Return a pointer to a buffer containing the term associated with the 
** entry that the iterator currently points to.
*/
static const u8 *fts5MultiIterTerm(Fts5MultiSegIter *pIter, int *pn){
  Fts5SegIter *p = &pIter->aSeg[ pIter->aFirst[1] ];
  *pn = p->term.n;
  return p->term.p;
}

/*
** Return true if the chunk iterator passed as the second argument is
** at EOF. Or if an error has already occurred. Otherwise, return false.
*/
static int fts5ChunkIterEof(Fts5Index *p, Fts5ChunkIter *pIter){
  return (p->rc || pIter->pLeaf==0);
}

/*
** Advance the chunk-iterator to the next chunk of data to read.
*/
static void fts5ChunkIterNext(Fts5Index *p, Fts5ChunkIter *pIter){
  assert( pIter->nRem>=pIter->n );
  pIter->nRem -= pIter->n;
  fts5DataRelease(pIter->pLeaf);
  pIter->pLeaf = 0;
  pIter->p = 0;
  if( pIter->nRem>0 ){
    Fts5Data *pLeaf;
    pIter->iLeafRowid++;
    pLeaf = pIter->pLeaf = fts5DataRead(p, pIter->iLeafRowid);
    if( pLeaf ){
      pIter->n = MIN(pIter->nRem, pLeaf->n-4);
      pIter->p = pLeaf->p+4;
    }
  }
}

/*
** Intialize the chunk iterator to read the position list data for which 
** the size field is at offset iOff of leaf pLeaf. 
*/
static void fts5ChunkIterInit(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegIter *pSeg,              /* Segment iterator to read poslist from */
  Fts5ChunkIter *pIter            /* Initialize this object */
){
  int iId = pSeg->pSeg->iSegid;
  i64 rowid = FTS5_SEGMENT_ROWID(pSeg->iIdx, iId, 0, pSeg->iLeafPgno);
  Fts5Data *pLeaf = pSeg->pLeaf;
  int iOff = pSeg->iLeafOffset;

  memset(pIter, 0, sizeof(*pIter));
  pIter->iLeafRowid = rowid;
  if( iOff<pLeaf->n ){
    fts5DataReference(pLeaf);
    pIter->pLeaf = pLeaf;
  }else{
    pIter->nRem = 1;
    fts5ChunkIterNext(p, pIter);
    if( p->rc ) return;
    iOff = 4;
    pLeaf = pIter->pLeaf;
  }

  iOff += getVarint32(&pLeaf->p[iOff], pIter->nRem);
  pIter->n = MIN(pLeaf->n - iOff, pIter->nRem);
  pIter->p = pLeaf->p + iOff;

  if( pIter->n==0 ){
    fts5ChunkIterNext(p, pIter);
  }
}

static void fts5ChunkIterRelease(Fts5ChunkIter *pIter){
  fts5DataRelease(pIter->pLeaf);
  pIter->pLeaf = 0;
}

/*
** Read and return the next 32-bit varint from the position-list iterator 
** passed as the second argument.
**
** If an error occurs, zero is returned an an error code left in 
** Fts5Index.rc. If an error has already occurred when this function is
** called, it is a no-op.
*/
static int fts5PosIterReadVarint(Fts5Index *p, Fts5PosIter *pIter){
  int iVal = 0;
  if( p->rc==SQLITE_OK ){
    if( pIter->iOff>=pIter->chunk.n ){
      fts5ChunkIterNext(p, &pIter->chunk);
      if( fts5ChunkIterEof(p, &pIter->chunk) ) return 0;
      pIter->iOff = 0;
    }
    pIter->iOff += getVarint32(&pIter->chunk.p[pIter->iOff], iVal);
  }
  return iVal;
}

/*
** Advance the position list iterator to the next entry.
*/
static void fts5PosIterNext(Fts5Index *p, Fts5PosIter *pIter){
  int iVal;
  assert( fts5ChunkIterEof(p, &pIter->chunk)==0 );
  iVal = fts5PosIterReadVarint(p, pIter);
  if( fts5ChunkIterEof(p, &pIter->chunk)==0 ){
    if( iVal==1 ){
      pIter->iCol = fts5PosIterReadVarint(p, pIter);
      pIter->iPos = fts5PosIterReadVarint(p, pIter) - 2;
    }else{
      pIter->iPos += (iVal - 2);
    }
  }
}

/*
** Initialize the Fts5PosIter object passed as the final argument to iterate
** through the position-list associated with the index entry that iterator 
** pMulti currently points to.
*/
static void fts5PosIterInit(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5MultiSegIter *pMulti,       /* Multi-seg iterator to read pos-list from */
  Fts5PosIter *pIter              /* Initialize this object */
){
  if( p->rc==SQLITE_OK ){
    Fts5SegIter *pSeg = &pMulti->aSeg[ pMulti->aFirst[1] ];
    memset(pIter, 0, sizeof(*pIter));
    fts5ChunkIterInit(p, pSeg, &pIter->chunk);
    if( fts5ChunkIterEof(p, &pIter->chunk)==0 ){
      fts5PosIterNext(p, pIter);
    }
  }
}

/*
** Return true if the position iterator passed as the second argument is
** at EOF. Or if an error has already occurred. Otherwise, return false.
*/
static int fts5PosIterEof(Fts5Index *p, Fts5PosIter *pIter){
  return (p->rc || pIter->chunk.pLeaf==0);
}


/*
** Allocate memory. The difference between this function and fts5IdxMalloc()
** is that this increments the Fts5Index.nPendingData variable by the
** number of bytes allocated. It should be used for all allocations used
** to store pending-data within the in-memory hash tables.
*/
static void *fts5PendingMalloc(Fts5Index *p, int nByte){
  p->nPendingData += nByte;
  return fts5IdxMalloc(p, nByte);
}

/*
** Add an entry for (iRowid/iCol/iPos) to the doclist for (pToken/nToken)
** in hash table for index iIdx. If iIdx is zero, this is the main terms 
** index. Values of 1 and greater for iIdx are prefix indexes.
**
** If an OOM error is encountered, set the Fts5Index.rc error code 
** accordingly.
*/
static void fts5AddTermToHash(
  Fts5Index *p,                   /* Index object to write to */
  int iIdx,                       /* Entry in p->aHash[] to update */
  int iCol,                       /* Column token appears in (-ve -> delete) */
  int iPos,                       /* Position of token within column */
  const char *pToken, int nToken  /* Token to add or remove to or from index */
){
  Fts5Config *pConfig = p->pConfig;
  Fts3Hash *pHash;
  Fts5PendingDoclist *pDoclist;
  Fts5PendingPoslist *pPoslist;
  i64 iRowid = p->iWriteRowid;     /* Rowid associated with these tokens */

  /* If an error has already occured this call is a no-op. */
  if( p->rc!=SQLITE_OK ) return;

  /* Find the hash table to use. It has already been allocated. */
  assert( iIdx<=pConfig->nPrefix );
  assert( iIdx==0 || nToken==pConfig->aPrefix[iIdx-1] );
  pHash = &p->aHash[iIdx];

  /* Find the doclist to append to. Allocate a new doclist object if
  ** required. */
  pDoclist = (Fts5PendingDoclist*)fts3HashFind(pHash, pToken, nToken);
  if( pDoclist==0 ){
    Fts5PendingDoclist *pDel;
    pDoclist = fts5PendingMalloc(p, sizeof(Fts5PendingDoclist) + nToken);
    if( pDoclist==0 ) return;
    pDoclist->pTerm = (u8*)&pDoclist[1];
    pDoclist->nTerm = nToken;
    memcpy(pDoclist->pTerm, pToken, nToken);
    pDel = fts3HashInsert(pHash, pDoclist->pTerm, nToken, pDoclist);
    if( pDel ){
      assert( pDoclist==pDel );
      sqlite3_free(pDel);
      p->rc = SQLITE_NOMEM;
      return;
    }
  }

  /* Find the poslist to append to. Allocate a new object if required. */
  pPoslist = pDoclist->pPoslist;
  if( pPoslist==0 || pPoslist->iRowid!=iRowid ){
    pPoslist = fts5PendingMalloc(p, sizeof(Fts5PendingPoslist));
    if( pPoslist==0 ) return;
    pPoslist->pNext = pDoclist->pPoslist;
    pPoslist->iRowid = iRowid;
    pDoclist->pPoslist = pPoslist;
    pDoclist->iCol = 0;
    pDoclist->iPos = 0;
  }

  /* Append the values to the position list. */
  if( iCol>=0 ){
    p->nPendingData -= pPoslist->buf.nSpace;
    if( iCol!=pDoclist->iCol ){
      fts5BufferAppendVarint(&p->rc, &pPoslist->buf, 1);
      fts5BufferAppendVarint(&p->rc, &pPoslist->buf, iCol);
      pDoclist->iCol = iCol;
      pDoclist->iPos = 0;
    }
    fts5BufferAppendVarint(&p->rc, &pPoslist->buf, iPos + 2 - pDoclist->iPos);
    p->nPendingData += pPoslist->buf.nSpace;
    pDoclist->iPos = iPos;
  }
}

/*
** Free the pending-doclist object passed as the only argument.
*/
static void fts5FreePendingDoclist(Fts5PendingDoclist *p){
  Fts5PendingPoslist *pPoslist;
  Fts5PendingPoslist *pNext;
  for(pPoslist=p->pPoslist; pPoslist; pPoslist=pNext){
    pNext = pPoslist->pNext;
    fts5BufferFree(&pPoslist->buf);
    sqlite3_free(pPoslist);
  }
  sqlite3_free(p);
}

/*
** Insert or remove data to or from the index. Each time a document is 
** added to or removed from the index, this function is called one or more
** times.
**
** For an insert, it must be called once for each token in the new document.
** If the operation is a delete, it must be called (at least) once for each
** unique token in the document with an iCol value less than zero. The iPos
** argument is ignored for a delete.
*/
void sqlite3Fts5IndexWrite(
  Fts5Index *p,                   /* Index to write to */
  int iCol,                       /* Column token appears in (-ve -> delete) */
  int iPos,                       /* Position of token within column */
  const char *pToken, int nToken  /* Token to add or remove to or from index */
){
  int i;                          /* Used to iterate through indexes */
  Fts5Config *pConfig = p->pConfig;

  /* If an error has already occured this call is a no-op. */
  if( p->rc!=SQLITE_OK ) return;

  /* Allocate hash tables if they have not already been allocated */
  if( p->aHash==0 ){
    int nHash = pConfig->nPrefix + 1;
    p->aHash = (Fts3Hash*)sqlite3_malloc(sizeof(Fts3Hash) * nHash);
    if( p->aHash==0 ){
      p->rc = SQLITE_NOMEM;
    }else{
      for(i=0; i<nHash; i++){
        fts3HashInit(&p->aHash[i], FTS3_HASH_STRING, 0);
      }
    }
  }

  /* Add the new token to the main terms hash table. And to each of the
  ** prefix hash tables that it is large enough for. */
  fts5AddTermToHash(p, 0, iCol, iPos, pToken, nToken);
  for(i=0; i<pConfig->nPrefix; i++){
    if( nToken>=pConfig->aPrefix[i] ){
      fts5AddTermToHash(p, i+1, iCol, iPos, pToken, pConfig->aPrefix[i]);
    }
  }
}

/*
** Allocate a new segment-id for the structure pStruct.
**
** If an error has already occurred, this function is a no-op. 0 is 
** returned in this case.
*/
static int fts5AllocateSegid(Fts5Index *p, Fts5Structure *pStruct){
  int i;
  if( p->rc!=SQLITE_OK ) return 0;

  for(i=0; i<100; i++){
    int iSegid;
    sqlite3_randomness(sizeof(int), (void*)&iSegid);
    iSegid = iSegid & ((1 << FTS5_DATA_ID_B)-1);
    if( iSegid ){
      int iLvl, iSeg;
      for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
        for(iSeg=0; iSeg<pStruct->aLevel[iLvl].nSeg; iSeg++){
          if( iSegid==pStruct->aLevel[iLvl].aSeg[iSeg].iSegid ){
            iSegid = 0;
          }
        }
      }
    }
    if( iSegid ) return iSegid;
  }

  p->rc = SQLITE_ERROR;
  return 0;
}

static Fts5PendingDoclist *fts5PendingMerge(
  Fts5Index *p, 
  Fts5PendingDoclist *pLeft,
  Fts5PendingDoclist *pRight
){
  Fts5PendingDoclist *p1 = pLeft;
  Fts5PendingDoclist *p2 = pRight;
  Fts5PendingDoclist *pRet = 0;
  Fts5PendingDoclist **ppOut = &pRet;

  while( p1 || p2 ){
    if( p1==0 ){
      *ppOut = p2;
      p2 = 0;
    }else if( p2==0 ){
      *ppOut = p1;
      p1 = 0;
    }else{
      int nCmp = MIN(p1->nTerm, p2->nTerm);
      int res = memcmp(p1->pTerm, p2->pTerm, nCmp);
      if( res==0 ) res = p1->nTerm - p2->nTerm;

      if( res>0 ){
        /* p2 is smaller */
        *ppOut = p2;
        ppOut = &p2->pNext;
        p2 = p2->pNext;
      }else{
        /* p1 is smaller */
        *ppOut = p1;
        ppOut = &p1->pNext;
        p1 = p1->pNext;
      }
      *ppOut = 0;
    }
  }

  return pRet;
}

/*
** Extract all tokens from hash table iHash and link them into a list
** in sorted order. The hash table is cleared before returning. It is
** the responsibility of the caller to free the elements of the returned
** list.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static Fts5PendingDoclist *fts5PendingList(Fts5Index *p, int iHash){
  const int nMergeSlot = 32;
  Fts3Hash *pHash;
  Fts3HashElem *pE;               /* Iterator variable */
  Fts5PendingDoclist **ap;
  Fts5PendingDoclist *pList;
  int i;

  ap = fts5IdxMalloc(p, sizeof(Fts5PendingDoclist*) * nMergeSlot);
  if( !ap ) return 0;

  pHash = &p->aHash[iHash];
  for(pE=fts3HashFirst(pHash); pE; pE=fts3HashNext(pE)){
    int i;
    Fts5PendingDoclist *pDoclist = (Fts5PendingDoclist*)fts3HashData(pE);
    assert( pDoclist->pNext==0 );
    for(i=0; ap[i]; i++){
      pDoclist = fts5PendingMerge(p, pDoclist, ap[i]);
      ap[i] = 0;
    }
    ap[i] = pDoclist;
  }

  pList = 0;
  for(i=0; i<nMergeSlot; i++){
    pList = fts5PendingMerge(p, pList, ap[i]);
  }

  sqlite3_free(ap);
  fts3HashClear(pHash);
  return pList;
}

/*
** Return the size of the prefix, in bytes, that buffer (nNew/pNew) shares
** with buffer (nOld/pOld).
*/
static int fts5PrefixCompress(
  int nOld, const u8 *pOld,
  int nNew, const u8 *pNew
){
  int i;
  for(i=0; i<nNew && i<nOld; i++){
    if( pOld[i]!=pNew[i] ) break;
  }
  return i;
}


/*
** This is called once for each leaf page except the first that contains
** at least one term. Argument (nTerm/pTerm) is the split-key - a term that
** is larger than all terms written to earlier leaves, and equal to or
** smaller than the first term on the new leaf.
**
** If an error occurs, an error code is left in Fts5Index.rc. If an error
** has already occurred when this function is called, it is a no-op.
*/
static void fts5WriteBtreeTerm(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegWriter *pWriter,         /* Writer object */
  int nTerm, const u8 *pTerm      /* First term on new page */
){
  int iHeight;
  for(iHeight=1; 1; iHeight++){
    Fts5PageWriter *pPage;

    if( iHeight>=pWriter->nWriter ){
      Fts5PageWriter *aNew;
      Fts5PageWriter *pNew;
      int nNew = sizeof(Fts5PageWriter) * (pWriter->nWriter+1);
      aNew = (Fts5PageWriter*)sqlite3_realloc(pWriter->aWriter, nNew);
      if( aNew==0 ) return;

      pNew = &aNew[pWriter->nWriter];
      memset(pNew, 0, sizeof(Fts5PageWriter));
      pNew->pgno = 1;
      fts5BufferAppendVarint(&p->rc, &pNew->buf, 1);

      pWriter->nWriter++;
      pWriter->aWriter = aNew;
    }
    pPage = &pWriter->aWriter[iHeight];

    if( pWriter->nEmpty ){
      assert( iHeight==1 );
      fts5BufferAppendVarint(&p->rc, &pPage->buf, 0);
      fts5BufferAppendVarint(&p->rc, &pPage->buf, pWriter->nEmpty);
      pWriter->nEmpty = 0;
    }

    if( pPage->buf.n>=p->pgsz ){
      /* pPage will be written to disk. The term will be written into the
      ** parent of pPage.  */
      i64 iRowid = FTS5_SEGMENT_ROWID(
          pWriter->iIdx, pWriter->iSegid, iHeight, pPage->pgno
      );
      fts5DataWrite(p, iRowid, pPage->buf.p, pPage->buf.n);
      fts5BufferZero(&pPage->buf);
      fts5BufferZero(&pPage->term);
      fts5BufferAppendVarint(&p->rc, &pPage->buf, pPage[-1].pgno);
      pPage->pgno++;
    }else{
      int nPre = fts5PrefixCompress(pPage->term.n, pPage->term.p, nTerm, pTerm);
      fts5BufferAppendVarint(&p->rc, &pPage->buf, nPre+2);
      fts5BufferAppendVarint(&p->rc, &pPage->buf, nTerm-nPre);
      fts5BufferAppendBlob(&p->rc, &pPage->buf, nTerm-nPre, pTerm+nPre);
      fts5BufferSet(&p->rc, &pPage->term, nTerm, pTerm);
      break;
    }
  }
}

static void fts5WriteBtreeNoTerm(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegWriter *pWriter          /* Writer object */
){
  pWriter->nEmpty++;
}

static void fts5WriteFlushLeaf(Fts5Index *p, Fts5SegWriter *pWriter){
  static const u8 zero[] = { 0x00, 0x00, 0x00, 0x00 };
  Fts5PageWriter *pPage = &pWriter->aWriter[0];
  i64 iRowid;

  if( pPage->term.n==0 ){
    /* No term was written to this page. */
    fts5WriteBtreeNoTerm(p, pWriter);
  }

  /* Write the current page to the db. */
  iRowid = FTS5_SEGMENT_ROWID(pWriter->iIdx, pWriter->iSegid, 0, pPage->pgno);
  fts5DataWrite(p, iRowid, pPage->buf.p, pPage->buf.n);

  /* Initialize the next page. */
  fts5BufferZero(&pPage->buf);
  fts5BufferZero(&pPage->term);
  fts5BufferAppendBlob(&p->rc, &pPage->buf, 4, zero);
  pPage->pgno++;

  /* Increase the leaves written counter */
  pWriter->nLeafWritten++;
}

/*
** Append term pTerm/nTerm to the segment being written by the writer passed
** as the second argument.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void fts5WriteAppendTerm(
  Fts5Index *p, 
  Fts5SegWriter *pWriter,
  int nTerm, const u8 *pTerm 
){
  int nPrefix;                    /* Bytes of prefix compression for term */
  Fts5PageWriter *pPage = &pWriter->aWriter[0];

  assert( pPage->buf.n==0 || pPage->buf.n>4 );
  if( pPage->buf.n==0 ){
    /* Zero the first term and first docid fields */
    static const u8 zero[] = { 0x00, 0x00, 0x00, 0x00 };
    fts5BufferAppendBlob(&p->rc, &pPage->buf, 4, zero);
    assert( pPage->term.n==0 );
  }
  if( p->rc ) return;
  
  if( pPage->term.n==0 ){
    /* Update the "first term" field of the page header. */
    assert( pPage->buf.p[2]==0 && pPage->buf.p[3]==0 );
    fts5PutU16(&pPage->buf.p[2], pPage->buf.n);
    nPrefix = 0;
    if( pWriter->aWriter[0].pgno!=1 ){
      fts5WriteBtreeTerm(p, pWriter, nTerm, pTerm);
      pPage = &pWriter->aWriter[0];
    }
  }else{
    nPrefix = fts5PrefixCompress(
        pPage->term.n, pPage->term.p, nTerm, pTerm
    );
    fts5BufferAppendVarint(&p->rc, &pPage->buf, nPrefix);
  }

  /* Append the number of bytes of new data, then the term data itself
  ** to the page. */
  fts5BufferAppendVarint(&p->rc, &pPage->buf, nTerm - nPrefix);
  fts5BufferAppendBlob(&p->rc, &pPage->buf, nTerm - nPrefix, &pTerm[nPrefix]);

  /* Update the Fts5PageWriter.term field. */
  fts5BufferSet(&p->rc, &pPage->term, nTerm, pTerm);

  pWriter->bFirstRowidInPage = 0;
  pWriter->bFirstRowidInDoclist = 1;

  /* If the current leaf page is full, flush it to disk. */
  if( pPage->buf.n>=p->pgsz ){
    fts5WriteFlushLeaf(p, pWriter);
    pWriter->bFirstRowidInPage = 1;
  }
}

/*
** Append a docid to the writers output. 
*/
static void fts5WriteAppendRowid(
  Fts5Index *p, 
  Fts5SegWriter *pWriter,
  i64 iRowid
){
  Fts5PageWriter *pPage = &pWriter->aWriter[0];

  /* If this is to be the first docid written to the page, set the 
  ** docid-pointer in the page-header.  */
  if( pWriter->bFirstRowidInPage ) fts5PutU16(pPage->buf.p, pPage->buf.n);

  /* Write the docid. */
  if( pWriter->bFirstRowidInDoclist || pWriter->bFirstRowidInPage ){
    fts5BufferAppendVarint(&p->rc, &pPage->buf, iRowid);
  }else{
    assert( iRowid<pWriter->iPrevRowid );
    fts5BufferAppendVarint(&p->rc, &pPage->buf, pWriter->iPrevRowid - iRowid);
  }
  pWriter->iPrevRowid = iRowid;
  pWriter->bFirstRowidInDoclist = 0;
  pWriter->bFirstRowidInPage = 0;

  if( pPage->buf.n>=p->pgsz ){
    fts5WriteFlushLeaf(p, pWriter);
    pWriter->bFirstRowidInPage = 1;
  }
}

static void fts5WriteAppendPoslistInt(
  Fts5Index *p, 
  Fts5SegWriter *pWriter,
  int iVal
){
  Fts5PageWriter *pPage = &pWriter->aWriter[0];
  fts5BufferAppendVarint(&p->rc, &pPage->buf, iVal);
  if( pPage->buf.n>=p->pgsz ){
    fts5WriteFlushLeaf(p, pWriter);
    pWriter->bFirstRowidInPage = 1;
  }
}

static void fts5WriteAppendZerobyte(Fts5Index *p, Fts5SegWriter *pWriter){
  fts5BufferAppendVarint(&p->rc, &pWriter->aWriter[0].buf, 0);
}

/*
** Write the contents of pending-doclist object pDoclist to writer pWriter.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void fts5WritePendingDoclist(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegWriter *pWriter,         /* Write to this writer object */
  Fts5PendingDoclist *pDoclist    /* Doclist to write to pWriter */
){
  Fts5PendingPoslist *pPoslist;   /* Used to iterate through the doclist */

  /* Append the term */
  fts5WriteAppendTerm(p, pWriter, pDoclist->nTerm, pDoclist->pTerm);

  /* Append the position list for each rowid */
  for(pPoslist=pDoclist->pPoslist; pPoslist; pPoslist=pPoslist->pNext){
    int i = 0;

    /* Append the rowid itself */
    fts5WriteAppendRowid(p, pWriter, pPoslist->iRowid);

    /* Append the size of the position list in bytes */
    fts5WriteAppendPoslistInt(p, pWriter, pPoslist->buf.n);

    /* Copy the position list to the output segment */
    while( i<pPoslist->buf.n){
      int iVal;
      i += getVarint32(&pPoslist->buf.p[i], iVal);
      fts5WriteAppendPoslistInt(p, pWriter, iVal);
    }
  }

  /* Write the doclist terminator */
  fts5WriteAppendZerobyte(p, pWriter);
}

static void fts5WriteFinish(
  Fts5Index *p, 
  Fts5SegWriter *pWriter, 
  int *pnHeight,
  int *pnLeaf
){
  int i;
  *pnLeaf = pWriter->aWriter[0].pgno;
  *pnHeight = pWriter->nWriter;
  fts5WriteFlushLeaf(p, pWriter);
  if( pWriter->nWriter>1 && pWriter->nEmpty ){
    Fts5PageWriter *pPg = &pWriter->aWriter[1];
    fts5BufferAppendVarint(&p->rc, &pPg->buf, 0);
    fts5BufferAppendVarint(&p->rc, &pPg->buf, pWriter->nEmpty);
  }
  for(i=1; i<pWriter->nWriter; i++){
    Fts5PageWriter *pPg = &pWriter->aWriter[i];
    i64 iRow = FTS5_SEGMENT_ROWID(pWriter->iIdx, pWriter->iSegid, i, pPg->pgno);
    fts5DataWrite(p, iRow, pPg->buf.p, pPg->buf.n);
  }
  for(i=0; i<pWriter->nWriter; i++){
    Fts5PageWriter *pPg = &pWriter->aWriter[i];
    fts5BufferFree(&pPg->term);
    fts5BufferFree(&pPg->buf);
  }
  sqlite3_free(pWriter->aWriter);
}

static void fts5WriteInit(
  Fts5Index *p, 
  Fts5SegWriter *pWriter, 
  int iIdx, int iSegid
){
  memset(pWriter, 0, sizeof(Fts5SegWriter));
  pWriter->iIdx = iIdx;
  pWriter->iSegid = iSegid;

  pWriter->aWriter = (Fts5PageWriter*)fts5IdxMalloc(p,sizeof(Fts5PageWriter));
  if( pWriter->aWriter==0 ) return;
  pWriter->nWriter = 1;
  pWriter->aWriter[0].pgno = 1;
}

static void fts5WriteInitForAppend(
  Fts5Index *p,                   /* FTS5 backend object */
  Fts5SegWriter *pWriter,         /* Writer to initialize */
  int iIdx,                       /* Index segment is a part of */
  Fts5StructureSegment *pSeg      /* Segment object to append to */
){
  int nByte = pSeg->nHeight * sizeof(Fts5PageWriter);
  memset(pWriter, 0, sizeof(Fts5SegWriter));
  pWriter->iIdx = iIdx;
  pWriter->iSegid = pSeg->iSegid;
  pWriter->aWriter = (Fts5PageWriter*)fts5IdxMalloc(p, nByte);
  pWriter->nWriter = pSeg->nHeight;

  if( p->rc==SQLITE_OK ){
    int pgno = 1;
    int i;
    pWriter->aWriter[0].pgno = pSeg->pgnoLast+1;
    for(i=pSeg->nHeight-1; i>0; i--){
      i64 iRowid = FTS5_SEGMENT_ROWID(pWriter->iIdx, pWriter->iSegid, i, pgno);
      Fts5PageWriter *pPg = &pWriter->aWriter[i];
      pPg->pgno = pgno;
      fts5DataBuffer(p, &pPg->buf, iRowid);
      if( p->rc==SQLITE_OK ){
        Fts5NodeIter ss;
        fts5NodeIterInit(pPg->buf.p, pPg->buf.n, &ss);
        while( ss.aData ) fts5NodeIterNext(&p->rc, &ss);
        fts5BufferSet(&p->rc, &pPg->term, ss.term.n, ss.term.p);
        pgno = ss.iChild;
        fts5NodeIterFree(&ss);
      }
    }
    if( pSeg->nHeight==1 ){
      pWriter->nEmpty = pSeg->pgnoLast-1;
    }
    assert( (pgno+pWriter->nEmpty)==pSeg->pgnoLast );
  }
}

/*
** Iterator pIter was used to iterate through the input segments of on an
** incremental merge operation. This function is called if the incremental
** merge step has finished but the input has not been completely exhausted.
*/
static void fts5TrimSegments(Fts5Index *p, Fts5MultiSegIter *pIter){
  int i;
  Fts5Buffer buf;
  memset(&buf, 0, sizeof(Fts5Buffer));
  for(i=0; i<pIter->nSeg; i++){
    Fts5SegIter *pSeg = &pIter->aSeg[i];
    if( pSeg->pSeg==0 ){
      /* no-op */
    }else if( pSeg->pLeaf==0 ){
      pSeg->pSeg->pgnoLast = 0;
      pSeg->pSeg->pgnoFirst = 0;
    }else{
      int iOff = pSeg->iTermLeafOffset;     /* Offset on new first leaf page */
      i64 iLeafRowid;
      Fts5Data *pData;
      int iId = pSeg->pSeg->iSegid;
      u8 aHdr[4] = {0x00, 0x00, 0x00, 0x04};

      iLeafRowid = FTS5_SEGMENT_ROWID(pSeg->iIdx, iId, 0, pSeg->iTermLeafPgno);
      pData = fts5DataRead(p, iLeafRowid);
      if( pData ){
        fts5BufferZero(&buf);
        fts5BufferAppendBlob(&p->rc, &buf, sizeof(aHdr), aHdr);
        fts5BufferAppendVarint(&p->rc, &buf, pSeg->term.n);
        fts5BufferAppendBlob(&p->rc, &buf, pSeg->term.n, pSeg->term.p);
        fts5BufferAppendBlob(&p->rc, &buf, pData->n - iOff, &pData->p[iOff]);
        fts5DataRelease(pData);
        pSeg->pSeg->pgnoFirst = pSeg->iTermLeafPgno;
        fts5DataDelete(p, FTS5_SEGMENT_ROWID(pSeg->iIdx, iId, 0, 1),iLeafRowid);
        fts5DataWrite(p, iLeafRowid, buf.p, buf.n);
      }
    }
  }
  fts5BufferFree(&buf);
}

/*
**
*/
static void fts5IndexMergeLevel(
  Fts5Index *p,                   /* FTS5 backend object */
  int iIdx,                       /* Index to work on */
  Fts5Structure *pStruct,         /* Stucture of index iIdx */
  int iLvl,                       /* Level to read input from */
  int *pnRem                      /* Write up to this many output leaves */
){
  Fts5StructureLevel *pLvl = &pStruct->aLevel[iLvl];
  Fts5StructureLevel *pLvlOut = &pStruct->aLevel[iLvl+1];
  Fts5MultiSegIter *pIter = 0;    /* Iterator to read input data */
  int nRem = *pnRem;              /* Output leaf pages left to write */
  int nInput;                     /* Number of input segments */
  Fts5SegWriter writer;           /* Writer object */
  Fts5StructureSegment *pSeg;     /* Output segment */
  Fts5Buffer term;
  int bRequireDoclistTerm = 0;

  assert( iLvl<pStruct->nLevel );
  assert( pLvl->nMerge<=pLvl->nSeg );

  memset(&writer, 0, sizeof(Fts5SegWriter));
  memset(&term, 0, sizeof(Fts5Buffer));
  writer.iIdx = iIdx;
  if( pLvl->nMerge ){
    assert( pLvlOut->nSeg>0 );
    nInput = pLvl->nMerge;
    fts5WriteInitForAppend(p, &writer, iIdx, &pLvlOut->aSeg[pLvlOut->nSeg-1]);
    pSeg = &pLvlOut->aSeg[pLvlOut->nSeg-1];
  }else{
    int iSegid = fts5AllocateSegid(p, pStruct);
    fts5WriteInit(p, &writer, iIdx, iSegid);

    /* Add the new segment to the output level */
    if( iLvl+1==pStruct->nLevel ) pStruct->nLevel++;
    pSeg = &pLvlOut->aSeg[pLvlOut->nSeg];
    pLvlOut->nSeg++;
    pSeg->pgnoFirst = 1;
    pSeg->iSegid = iSegid;

    /* Read input from all segments in the input level */
    nInput = pLvl->nSeg;
  }
#if 0
fprintf(stdout, "merging %d segments from level %d!", nInput, iLvl);
fflush(stdout);
#endif

  for(fts5MultiIterNew(p, pStruct, iIdx, 0, 0, iLvl, nInput, &pIter);
      fts5MultiIterEof(p, pIter)==0;
      fts5MultiIterNext(p, pIter)
  ){
    Fts5SegIter *pSeg = &pIter->aSeg[ pIter->aFirst[1] ];
    Fts5ChunkIter sPos;           /* Used to iterate through position list */
    int nTerm;
    const u8 *pTerm = fts5MultiIterTerm(pIter, &nTerm);

    if( nTerm!=term.n || memcmp(pTerm, term.p, nTerm) ){
      if( writer.nLeafWritten>nRem ) break;

      /* This is a new term. Append a term to the output segment. */
      if( bRequireDoclistTerm ){
        fts5WriteAppendZerobyte(p, &writer);
      }
      fts5WriteAppendTerm(p, &writer, nTerm, pTerm);
      fts5BufferSet(&p->rc, &term, nTerm, pTerm);
      bRequireDoclistTerm = 1;
    }

    /* Append the rowid to the output */
    fts5WriteAppendRowid(p, &writer, fts5MultiIterRowid(pIter));

    /* Copy the position list from input to output */
    fts5ChunkIterInit(p, pSeg, &sPos);
    fts5WriteAppendPoslistInt(p, &writer, sPos.nRem);
    for(/* noop */; fts5ChunkIterEof(p, &sPos)==0; fts5ChunkIterNext(p, &sPos)){
      int iOff = 0;
      while( iOff<sPos.n ){
        int iVal;
        iOff += getVarint32(&sPos.p[iOff], iVal);
        fts5WriteAppendPoslistInt(p, &writer, iVal);
      }
    }
  }

  /* Flush the last leaf page to disk. Set the output segment b-tree height
  ** and last leaf page number at the same time.  */
  fts5WriteFinish(p, &writer, &pSeg->nHeight, &pSeg->pgnoLast);

  if( fts5MultiIterEof(p, pIter) ){
    int i;

    /* Remove the redundant segments from the %_data table */
    for(i=0; i<nInput; i++){
      fts5DataRemoveSegment(p, iIdx, pLvl->aSeg[i].iSegid);
    }

    /* Remove the redundant segments from the input level */
    if( pLvl->nSeg!=nInput ){
      int nMove = (pLvl->nSeg - nInput) * sizeof(Fts5StructureSegment);
      memmove(pLvl->aSeg, &pLvl->aSeg[nInput], nMove);
    }
    pLvl->nSeg -= nInput;
    pLvl->nMerge = 0;
  }else{
    fts5TrimSegments(p, pIter);
    pLvl->nMerge = nInput;
  }

  fts5MultiIterFree(p, pIter);
  fts5BufferFree(&term);
  *pnRem -= writer.nLeafWritten;
}

/*
** A total of nLeaf leaf pages of data has just been flushed to a level-0
** segments in index iIdx with structure pStruct. This function updates the
** write-counter accordingly and, if necessary, performs incremental merge
** work.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void fts5IndexWork(
  Fts5Index *p,                   /* FTS5 backend object */
  int iIdx,                       /* Index to work on */
  Fts5Structure *pStruct,         /* Current structure of index */
  int nLeaf                       /* Number of output leaves just written */
){
  i64 nWrite;                     /* Initial value of write-counter */
  int nWork;                      /* Number of work-quanta to perform */
  int nRem;                       /* Number of leaf pages left to write */

  /* Update the write-counter. While doing so, set nWork. */
  nWrite = pStruct->nWriteCounter;
  nWork = ((nWrite + nLeaf) / p->nWorkUnit) - (nWrite / p->nWorkUnit);
  pStruct->nWriteCounter += nLeaf;
  nRem = p->nWorkUnit * nWork * pStruct->nLevel;

  while( nRem>0 ){
    int iLvl;                   /* To iterate through levels */
    int iBestLvl = -1;          /* Level offering the most input segments */
    int nBest = 0;              /* Number of input segments on best level */

    /* Set iBestLvl to the level to read input segments from. */
    for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
      Fts5StructureLevel *pLvl = &pStruct->aLevel[iLvl];
      if( pLvl->nMerge ){
        if( pLvl->nMerge>nBest ){
          iBestLvl = iLvl;
          nBest = pLvl->nMerge;
        }
        break;
      }
      if( pLvl->nSeg>nBest ){
        nBest = pLvl->nSeg;
        iBestLvl = iLvl;
      }
    }
    assert( iBestLvl>=0 && nBest>0 );

    if( nBest<p->nMinMerge && pStruct->aLevel[iBestLvl].nMerge==0 ) break;
    fts5IndexMergeLevel(p, iIdx, pStruct, iBestLvl, &nRem);
    assert( nRem==0 || p->rc==SQLITE_OK );
  }
}

/*
** Flush the contents of in-memory hash table iHash to a new level-0 
** segment on disk. Also update the corresponding structure record.
**
** If an error occurs, set the Fts5Index.rc error code. If an error has 
** already occurred, this function is a no-op.
*/
static void fts5FlushOneHash(Fts5Index *p, int iHash, int *pnLeaf){
  Fts5Structure *pStruct;
  int iSegid;
  int pgnoLast = 0;                 /* Last leaf page number in segment */

  /* Obtain a reference to the index structure and allocate a new segment-id
  ** for the new level-0 segment.  */
  pStruct = fts5StructureRead(p, iHash);
  iSegid = fts5AllocateSegid(p, pStruct);

  if( iSegid ){
    Fts5SegWriter writer;
    Fts5PendingDoclist *pList;
    Fts5PendingDoclist *pIter;
    Fts5PendingDoclist *pNext;

    Fts5StructureSegment *pSeg;   /* New segment within pStruct */
    int nHeight;                  /* Height of new segment b-tree */

    pList = fts5PendingList(p, iHash);
    assert( pList!=0 || p->rc!=SQLITE_OK );
    fts5WriteInit(p, &writer, iHash, iSegid);

    for(pIter=pList; pIter; pIter=pNext){
      pNext = pIter->pNext;
      fts5WritePendingDoclist(p, &writer, pIter);
      fts5FreePendingDoclist(pIter);
    }
    fts5WriteFinish(p, &writer, &nHeight, &pgnoLast);

    /* Edit the Fts5Structure and write it back to the database. */
    if( pStruct->nLevel==0 ) pStruct->nLevel = 1;
    pSeg = &pStruct->aLevel[0].aSeg[ pStruct->aLevel[0].nSeg++ ];
    pSeg->iSegid = iSegid;
    pSeg->nHeight = nHeight;
    pSeg->pgnoFirst = 1;
    pSeg->pgnoLast = pgnoLast;
  }

  fts5IndexWork(p, iHash, pStruct, pgnoLast);
  fts5StructureWrite(p, iHash, pStruct);
  fts5StructureRelease(pStruct);
}

/*
** Indicate that all subsequent calls to sqlite3Fts5IndexWrite() pertain
** to the document with rowid iRowid.
*/
void sqlite3Fts5IndexBeginWrite(Fts5Index *p, i64 iRowid){
  if( iRowid<=p->iWriteRowid ){
    sqlite3Fts5IndexFlush(p);
  }
  p->iWriteRowid = iRowid;
}

/*
** Flush any data stored in the in-memory hash tables to the database.
*/
void sqlite3Fts5IndexFlush(Fts5Index *p){
  Fts5Config *pConfig = p->pConfig;
  int i;                          /* Used to iterate through indexes */
  int nLeaf = 0;                  /* Number of leaves written */

  /* If an error has already occured this call is a no-op. */
  if( p->rc!=SQLITE_OK || p->nPendingData==0 ) return;
  assert( p->aHash );

  /* Flush the terms and each prefix index to disk */
  for(i=0; i<=pConfig->nPrefix; i++){
    fts5FlushOneHash(p, i, &nLeaf);
  }
  p->nPendingData = 0;
}

/*
** Commit data to disk.
*/
int sqlite3Fts5IndexSync(Fts5Index *p){
  sqlite3Fts5IndexFlush(p);
  fts5CloseReader(p);
  return p->rc;
}

/*
** Discard any data stored in the in-memory hash tables. Do not write it
** to the database. Additionally, assume that the contents of the %_data
** table may have changed on disk. So any in-memory caches of %_data 
** records must be invalidated.
*/
int sqlite3Fts5IndexRollback(Fts5Index *p){
  fts5CloseReader(p);
  return SQLITE_OK;
}

/*
** Open a new Fts5Index handle. If the bCreate argument is true, create
** and initialize the underlying %_data table.
**
** If successful, set *pp to point to the new object and return SQLITE_OK.
** Otherwise, set *pp to NULL and return an SQLite error code.
*/
int sqlite3Fts5IndexOpen(
  Fts5Config *pConfig, 
  int bCreate, 
  Fts5Index **pp,
  char **pzErr
){
  int rc = SQLITE_OK;
  Fts5Index *p;                   /* New object */

  *pp = p = (Fts5Index*)sqlite3_malloc(sizeof(Fts5Index));
  if( !p ) return SQLITE_NOMEM;

  memset(p, 0, sizeof(Fts5Index));
  p->pConfig = pConfig;
  p->pgsz = 1000;
  p->nMinMerge = FTS5_MIN_MERGE;
  p->nWorkUnit = FTS5_WORK_UNIT;
  p->nMaxPendingData = 1024*1024;
  p->zDataTbl = sqlite3_mprintf("%s_data", pConfig->zName);
  if( p->zDataTbl==0 ){
    rc = SQLITE_NOMEM;
  }else if( bCreate ){
    int i;
    Fts5Structure s;
    rc = sqlite3Fts5CreateTable(
        pConfig, "data", "id INTEGER PRIMARY KEY, block BLOB", pzErr
    );
    if( rc==SQLITE_OK ){
      memset(&s, 0, sizeof(Fts5Structure));
      for(i=0; i<pConfig->nPrefix+1; i++){
        fts5StructureWrite(p, i, &s);
      }
      rc = p->rc;
    }
  }

  if( rc ){
    sqlite3Fts5IndexClose(p, 0);
    *pp = 0;
  }
  return rc;
}

/*
** Close a handle opened by an earlier call to sqlite3Fts5IndexOpen().
*/
int sqlite3Fts5IndexClose(Fts5Index *p, int bDestroy){
  int rc = SQLITE_OK;
  if( bDestroy ){
    rc = sqlite3Fts5DropTable(p->pConfig, "data");
  }
  assert( p->pReader==0 );
  sqlite3_finalize(p->pWriter);
  sqlite3_finalize(p->pDeleter);
  sqlite3_free(p->aHash);
  sqlite3_free(p->zDataTbl);
  sqlite3_free(p);
  return rc;
}

/*
** Return a simple checksum value based on the arguments.
*/
static u64 fts5IndexEntryCksum(
  i64 iRowid, 
  int iCol, 
  int iPos, 
  const char *pTerm, 
  int nTerm
){
  int i;
  u64 ret = iRowid;
  ret += (ret<<3) + iCol;
  ret += (ret<<3) + iPos;
  for(i=0; i<nTerm; i++) ret += (ret<<3) + pTerm[i];
  return ret;
}

/*
** Calculate and return a checksum that is the XOR of the index entry
** checksum of all entries that would be generated by the token specified
** by the final 5 arguments.
*/
u64 sqlite3Fts5IndexCksum(
  Fts5Config *pConfig,            /* Configuration object */
  i64 iRowid,                     /* Document term appears in */
  int iCol,                       /* Column term appears in */
  int iPos,                       /* Position term appears in */
  const char *pTerm, int nTerm    /* Term at iPos */
){
  u64 ret = 0;                    /* Return value */
  int iIdx;                       /* For iterating through indexes */

  for(iIdx=0; iIdx<=pConfig->nPrefix; iIdx++){
    int n = ((iIdx==pConfig->nPrefix) ? nTerm : pConfig->aPrefix[iIdx]);
    if( n<=nTerm ){
      ret ^= fts5IndexEntryCksum(iRowid, iCol, iPos, pTerm, n);
    }
  }

  return ret;
}

static void fts5BtreeIterInit(
  Fts5Index *p, 
  int iIdx,
  Fts5StructureSegment *pSeg, 
  Fts5BtreeIter *pIter
){
  int nByte;
  int i;
  nByte = sizeof(pIter->aLvl[0]) * (pSeg->nHeight-1);
  memset(pIter, 0, sizeof(*pIter));
  pIter->nLvl = pSeg->nHeight-1;
  pIter->iIdx = iIdx;
  pIter->p = p;
  pIter->pSeg = pSeg;
  if( nByte && p->rc==SQLITE_OK ){
    pIter->aLvl = (Fts5BtreeIterLevel*)fts5IdxMalloc(p, nByte);
  }
  for(i=0; p->rc==SQLITE_OK && i<pIter->nLvl; i++){
    i64 iRowid = FTS5_SEGMENT_ROWID(iIdx, pSeg->iSegid, i+1, 1);
    Fts5Data *pData;
    pIter->aLvl[i].pData = pData = fts5DataRead(p, iRowid);
    if( pData ){
      fts5NodeIterInit(pData->p, pData->n, &pIter->aLvl[i].s);
    }
  }

  if( pIter->nLvl==0 || p->rc ){
    pIter->bEof = 1;
    pIter->iLeaf = pSeg->pgnoLast;
  }else{
    pIter->nEmpty = pIter->aLvl[0].s.nEmpty;
    pIter->iLeaf = pIter->aLvl[0].s.iChild;
  }
}

static void fts5BtreeIterNext(Fts5BtreeIter *pIter){
  Fts5Index *p = pIter->p;
  int i;

  assert( pIter->bEof==0 && pIter->aLvl[0].s.aData );
  for(i=0; i<pIter->nLvl && p->rc==SQLITE_OK; i++){
    Fts5BtreeIterLevel *pLvl = &pIter->aLvl[i];
    fts5NodeIterNext(&p->rc, &pLvl->s);
    if( pLvl->s.aData ){
      fts5BufferSet(&p->rc, &pIter->term, pLvl->s.term.n, pLvl->s.term.p);
      break;
    }else{
      fts5NodeIterFree(&pLvl->s);
      fts5DataRelease(pLvl->pData);
      pLvl->pData = 0;
    }
  }
  if( i==pIter->nLvl || p->rc ){
    pIter->bEof = 1;
  }else{
    int iSegid = pIter->pSeg->iSegid;
    for(i--; i>=0; i--){
      Fts5BtreeIterLevel *pLvl = &pIter->aLvl[i];
      i64 iRowid = FTS5_SEGMENT_ROWID(pIter->iIdx,iSegid,i+1,pLvl[1].s.iChild);
      pLvl->pData = fts5DataRead(p, iRowid);
      if( pLvl->pData ){
        fts5NodeIterInit(pLvl->pData->p, pLvl->pData->n, &pLvl->s);
      }
    }
  }

  pIter->nEmpty = pIter->aLvl[0].s.nEmpty;
  pIter->iLeaf = pIter->aLvl[0].s.iChild;
  assert( p->rc==SQLITE_OK || pIter->bEof );
}

static void fts5BtreeIterFree(Fts5BtreeIter *pIter){
  int i;
  for(i=0; i<pIter->nLvl; i++){
    Fts5BtreeIterLevel *pLvl = &pIter->aLvl[i];
    fts5NodeIterFree(&pLvl->s);
    if( pLvl->pData ){
      fts5DataRelease(pLvl->pData);
      pLvl->pData = 0;
    }
  }
  sqlite3_free(pIter->aLvl);
  fts5BufferFree(&pIter->term);
}

static void fts5IndexIntegrityCheckSegment(
  Fts5Index *p,                   /* FTS5 backend object */
  int iIdx,                       /* Index that pSeg is a part of */
  Fts5StructureSegment *pSeg      /* Segment to check internal consistency */
){
  Fts5BtreeIter iter;             /* Used to iterate through b-tree hierarchy */

  /* Iterate through the b-tree hierarchy.  */
  for(fts5BtreeIterInit(p, iIdx, pSeg, &iter);
      iter.bEof==0;
      fts5BtreeIterNext(&iter)
  ){
    i64 iRow;                     /* Rowid for this leaf */
    Fts5Data *pLeaf;              /* Data for this leaf */
    int iOff;                     /* Offset of first term on leaf */
    int i;                        /* Used to iterate through empty leaves */

    /* If the leaf in question has already been trimmed from the segment, 
    ** ignore this b-tree entry. Otherwise, load it into memory. */
    if( iter.iLeaf<pSeg->pgnoFirst ) continue;
    iRow = FTS5_SEGMENT_ROWID(iIdx, pSeg->iSegid, 0, iter.iLeaf);
    pLeaf = fts5DataRead(p, iRow);
    if( pLeaf==0 ) break;

    /* Check that the leaf contains at least one term, and that it is equal
    ** to or larger than the split-key in iter.term.  */
    iOff = fts5GetU16(&pLeaf->p[2]);
    if( iOff==0 ){
      p->rc = FTS5_CORRUPT;
    }else{
      int nTerm;                  /* Size of term on leaf in bytes */
      int res;                    /* Comparison of term and split-key */
      iOff += getVarint32(&pLeaf->p[iOff], nTerm);
      res = memcmp(&pLeaf->p[iOff], iter.term.p, MIN(nTerm, iter.term.n));
      if( res==0 ) res = nTerm - iter.term.n;
      if( res<0 ){
        p->rc = FTS5_CORRUPT;
      }
    }
    fts5DataRelease(pLeaf);
    if( p->rc ) break;

    /* Now check that the iter.nEmpty leaves following the current leaf
    ** (a) exist and (b) contain no terms. */
    for(i=1; i<=iter.nEmpty; i++){
      pLeaf = fts5DataRead(p, iRow+i);
      if( pLeaf && 0!=fts5GetU16(&pLeaf->p[2]) ){
        p->rc = FTS5_CORRUPT;
      }
      fts5DataRelease(pLeaf);
    }
  }

  if( p->rc==SQLITE_OK && iter.iLeaf!=pSeg->pgnoLast ){
    p->rc = FTS5_CORRUPT;
  }

  fts5BtreeIterFree(&iter);
}

/*
** Run internal checks to ensure that the FTS index (a) is internally 
** consistent and (b) contains entries for which the XOR of the checksums
** as calculated by fts5IndexEntryCksum() is cksum.
**
** Return SQLITE_CORRUPT if any of the internal checks fail, or if the
** checksum does not match. Return SQLITE_OK if all checks pass without
** error, or some other SQLite error code if another error (e.g. OOM)
** occurs.
*/
int sqlite3Fts5IndexIntegrityCheck(Fts5Index *p, u64 cksum){
  Fts5Config *pConfig = p->pConfig;
  int iIdx;                       /* Used to iterate through indexes */
  int rc;                         /* Return code */
  u64 cksum2 = 0;                 /* Checksum based on contents of indexes */

  /* Check that the checksum of the index matches the argument checksum */
  for(iIdx=0; iIdx<=pConfig->nPrefix; iIdx++){
    Fts5MultiSegIter *pIter;
    Fts5Structure *pStruct = fts5StructureRead(p, iIdx);
    for(fts5MultiIterNew(p, pStruct, iIdx, 0, 0, -1, 0, &pIter);
        fts5MultiIterEof(p, pIter)==0;
        fts5MultiIterNext(p, pIter)
    ){
      Fts5PosIter sPos;           /* Used to iterate through position list */
      int n;                      /* Size of term in bytes */
      i64 iRowid = fts5MultiIterRowid(pIter);
      char *z = (char*)fts5MultiIterTerm(pIter, &n);

      for(fts5PosIterInit(p, pIter, &sPos);
          fts5PosIterEof(p, &sPos)==0;
          fts5PosIterNext(p, &sPos)
      ){
        cksum2 ^= fts5IndexEntryCksum(iRowid, sPos.iCol, sPos.iPos, z, n);
#if 0
        fprintf(stdout, "rowid=%d ", (int)iRowid);
        fprintf(stdout, "term=%.*s ", n, z);
        fprintf(stdout, "col=%d ", sPos.iCol);
        fprintf(stdout, "off=%d\n", sPos.iPos);
        fflush(stdout);
#endif
      }
    }
    fts5MultiIterFree(p, pIter);
    fts5StructureRelease(pStruct);
  }
  rc = p->rc;
  if( rc==SQLITE_OK && cksum!=cksum2 ) rc = FTS5_CORRUPT;

  /* Check that the internal nodes of each segment match the leaves */
  for(iIdx=0; rc==SQLITE_OK && iIdx<=pConfig->nPrefix; iIdx++){
    Fts5Structure *pStruct = fts5StructureRead(p, iIdx);
    if( pStruct ){
      int iLvl, iSeg;
      for(iLvl=0; iLvl<pStruct->nLevel; iLvl++){
        for(iSeg=0; iSeg<pStruct->aLevel[iLvl].nSeg; iSeg++){
          Fts5StructureSegment *pSeg = &pStruct->aLevel[iLvl].aSeg[iSeg];
          fts5IndexIntegrityCheckSegment(p, iIdx, pSeg);
        }
      }
    }
    fts5StructureRelease(pStruct);
    rc = p->rc;
  }

  return rc;
}

/*
*/
static void fts5DecodeStructure(
  int *pRc,                       /* IN/OUT: error code */
  Fts5Buffer *pBuf,
  const u8 *pBlob, int nBlob
){
  int rc;                         /* Return code */
  int iLvl, iSeg;                 /* Iterate through levels, segments */
  Fts5Structure *p = 0;           /* Decoded structure object */

  rc = fts5StructureDecode(pBlob, nBlob, &p);
  if( rc!=SQLITE_OK ){
    *pRc = rc;
    return;
  }

  for(iLvl=0; iLvl<p->nLevel; iLvl++){
    Fts5StructureLevel *pLvl = &p->aLevel[iLvl];
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, 
        " {lvl=%d nMerge=%d", iLvl, pLvl->nMerge
    );
    for(iSeg=0; iSeg<pLvl->nSeg; iSeg++){
      Fts5StructureSegment *pSeg = &pLvl->aSeg[iSeg];
      sqlite3Fts5BufferAppendPrintf(pRc, pBuf, 
          " {id=%d h=%d leaves=%d..%d}", pSeg->iSegid, pSeg->nHeight, 
          pSeg->pgnoFirst, pSeg->pgnoLast
      );
    }
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, "}");
  }

  fts5StructureRelease(p);
}

/*
** Decode a segment-data rowid from the %_data table. This function is
** the opposite of macro FTS5_SEGMENT_ROWID().
*/
static void fts5DecodeRowid(
  i64 iRowid,                     /* Rowid from %_data table */
  int *piIdx,                     /* OUT: Index */
  int *piSegid,                   /* OUT: Segment id */
  int *piHeight,                  /* OUT: Height */
  int *piPgno                     /* OUT: Page number */
){
  *piPgno = (int)(iRowid & (((i64)1 << FTS5_DATA_PAGE_B) - 1));
  iRowid >>= FTS5_DATA_PAGE_B;

  *piHeight = (int)(iRowid & (((i64)1 << FTS5_DATA_HEIGHT_B) - 1));
  iRowid >>= FTS5_DATA_HEIGHT_B;

  *piSegid = (int)(iRowid & (((i64)1 << FTS5_DATA_ID_B) - 1));
  iRowid >>= FTS5_DATA_ID_B;

  *piIdx = (int)(iRowid & (((i64)1 << FTS5_DATA_IDX_B) - 1));
}

/*
** Buffer (a/n) is assumed to contain a list of serialized varints. Read
** each varint and append its string representation to buffer pBuf. Return
** after either the input buffer is exhausted or a 0 value is read.
**
** The return value is the number of bytes read from the input buffer.
*/
static int fts5DecodePoslist(int *pRc, Fts5Buffer *pBuf, const u8 *a, int n){
  int iOff = 0;
  while( iOff<n ){
    int iVal;
    iOff += getVarint32(&a[iOff], iVal);
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " %d", iVal);
  }
  return iOff;
}

/*
** The start of buffer (a/n) contains the start of a doclist. The doclist
** may or may not finish within the buffer. This function appends a text
** representation of the part of the doclist that is present to buffer
** pBuf. 
**
** The return value is the number of bytes read from the input buffer.
*/
static int fts5DecodeDoclist(int *pRc, Fts5Buffer *pBuf, const u8 *a, int n){
  i64 iDocid;
  int iOff = 0;

  if( iOff<n ){
    iOff += sqlite3GetVarint(&a[iOff], (u64*)&iDocid);
    sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " rowid=%lld", iDocid);
  }
  while( iOff<n ){
    int nPos;
    iOff += getVarint32(&a[iOff], nPos);
    iOff += fts5DecodePoslist(pRc, pBuf, &a[iOff], MIN(n-iOff, nPos));
    if( iOff<n ){
      i64 iDelta;
      iOff += sqlite3GetVarint(&a[iOff], (u64*)&iDelta);
      if( iDelta==0 ) return iOff;
      iDocid -= iDelta;
      sqlite3Fts5BufferAppendPrintf(pRc, pBuf, " rowid=%lld", iDocid);
    }
  }

  return iOff;
}

/*
** The implementation of user-defined scalar function fts5_decode().
*/
static void fts5DecodeFunction(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args (always 2) */
  sqlite3_value **apVal           /* Function arguments */
){
  i64 iRowid;                     /* Rowid for record being decoded */
  int iIdx,iSegid,iHeight,iPgno;  /* Rowid compenents */
  const u8 *a; int n;             /* Record to decode */
  Fts5Buffer s;                   /* Build up text to return here */
  int rc = SQLITE_OK;             /* Return code */

  assert( nArg==2 );
  memset(&s, 0, sizeof(Fts5Buffer));
  iRowid = sqlite3_value_int64(apVal[0]);
  n = sqlite3_value_bytes(apVal[1]);
  a = sqlite3_value_blob(apVal[1]);
  fts5DecodeRowid(iRowid, &iIdx, &iSegid, &iHeight, &iPgno);

  if( iSegid==0 ){
    if( iRowid==FTS5_AVERAGES_ROWID ){
      sqlite3Fts5BufferAppendPrintf(&rc, &s, "{averages} ");
    }else{
      sqlite3Fts5BufferAppendPrintf(&rc, &s, 
          "{structure idx=%d}", (int)(iRowid-10)
      );
      fts5DecodeStructure(&rc, &s, a, n);
    }
  }else{

    Fts5Buffer term;
    memset(&term, 0, sizeof(Fts5Buffer));
    sqlite3Fts5BufferAppendPrintf(&rc, &s, "(idx=%d segid=%d h=%d pgno=%d) ",
        iIdx, iSegid, iHeight, iPgno
    );

    if( iHeight==0 ){
      int iTermOff = 0;
      int iRowidOff = 0;
      int iOff;
      int nKeep = 0;

      iRowidOff = fts5GetU16(&a[0]);
      iTermOff = fts5GetU16(&a[2]);

      if( iRowidOff ){
        iOff = iRowidOff;
      }else if( iTermOff ){
        iOff = iTermOff;
      }else{
        iOff = n;
      }
      fts5DecodePoslist(&rc, &s, &a[4], iOff-4);


      assert( iRowidOff==0 || iOff==iRowidOff );
      if( iRowidOff ){
        iOff += fts5DecodeDoclist(&rc, &s, &a[iOff], n-iOff);
      }

      assert( iTermOff==0 || iOff==iTermOff );
      while( iOff<n ){
        int nByte;
        iOff += getVarint32(&a[iOff], nByte);
        term.n= nKeep;
        fts5BufferAppendBlob(&rc, &term, nByte, &a[iOff]);
        iOff += nByte;

        sqlite3Fts5BufferAppendPrintf(
            &rc, &s, " term=%.*s", term.n, (const char*)term.p
        );
        iOff += fts5DecodeDoclist(&rc, &s, &a[iOff], n-iOff);
        if( iOff<n ){
          iOff += getVarint32(&a[iOff], nKeep);
        }
      }
      fts5BufferFree(&term);
    }else{
      Fts5NodeIter ss;
      for(fts5NodeIterInit(a, n, &ss); ss.aData; fts5NodeIterNext(&rc, &ss)){
        if( ss.term.n==0 ){
          sqlite3Fts5BufferAppendPrintf(&rc, &s, " left=%d", ss.iChild);
        }else{
          sqlite3Fts5BufferAppendPrintf(&rc,&s, " \"%.*s\"", 
              ss.term.n, ss.term.p
          );
        }
        if( ss.nEmpty ){
          sqlite3Fts5BufferAppendPrintf(&rc, &s, " empty=%d", ss.nEmpty);
        }
      }
      fts5NodeIterFree(&ss);
    }
  }
  
  if( rc==SQLITE_OK ){
    sqlite3_result_text(pCtx, (const char*)s.p, s.n, SQLITE_TRANSIENT);
  }else{
    sqlite3_result_error_code(pCtx, rc);
  }
  fts5BufferFree(&s);
}

/*
** This is called as part of registering the FTS5 module with database
** connection db. It registers several user-defined scalar functions useful
** with FTS5.
**
** If successful, SQLITE_OK is returned. If an error occurs, some other
** SQLite error code is returned instead.
*/
int sqlite3Fts5IndexInit(sqlite3 *db){
  int rc = sqlite3_create_function(
      db, "fts5_decode", 2, SQLITE_UTF8, 0, fts5DecodeFunction, 0, 0
  );
  return rc;
}

/*
** Set the target page size for the index object.
*/
void sqlite3Fts5IndexPgsz(Fts5Index *p, int pgsz){
  p->pgsz = pgsz;
}

/*
** Open a new iterator to iterate though all docids that match the 
** specified token or token prefix.
*/
Fts5IndexIter *sqlite3Fts5IndexQuery(
  Fts5Index *p,                   /* FTS index to query */
  const char *pToken, int nToken, /* Token (or prefix) to query for */
  int flags                       /* Mask of FTS5INDEX_QUERY_X flags */
){
  Fts5IndexIter *pRet;
  int iIdx = 0;

  if( flags & FTS5INDEX_QUERY_PREFIX ){
    Fts5Config *pConfig = p->pConfig;
    for(iIdx=1; iIdx<=pConfig->nPrefix; iIdx++){
      if( pConfig->aPrefix[iIdx-1]==nToken ) break;
    }
    if( iIdx>pConfig->nPrefix ){
      /* No matching prefix index. todo: deal with this. */
      assert( 0 );
    }
  }

  pRet = (Fts5IndexIter*)sqlite3_malloc(sizeof(Fts5IndexIter));
  if( pRet ){
    memset(pRet, 0, sizeof(Fts5IndexIter));
    pRet->pStruct = fts5StructureRead(p, 0);
    if( pRet->pStruct ){
      fts5MultiIterNew(p, 
          pRet->pStruct, iIdx, (const u8*)pToken, nToken, -1, 0, &pRet->pMulti
      );
    }
    pRet->pIndex = p;
  }

  if( p->rc ){
    sqlite3Fts5IterClose(pRet);
    pRet = 0;
  }
  return pRet;
}

/*
** Return true if the iterator passed as the only argument is at EOF.
*/
int sqlite3Fts5IterEof(Fts5IndexIter *pIter){
  return fts5MultiIterEof(pIter->pIndex, pIter->pMulti);
}

/*
** Move to the next matching rowid. 
*/
void sqlite3Fts5IterNext(Fts5IndexIter *pIter, i64 iMatch){
  fts5BufferZero(&pIter->poslist);
  fts5MultiIterNext(pIter->pIndex, pIter->pMulti);
}

/*
** Return the current rowid.
*/
i64 sqlite3Fts5IterRowid(Fts5IndexIter *pIter){
  return fts5MultiIterRowid(pIter->pMulti);
}

/*
** Return a pointer to a buffer containing a copy of the position list for
** the current entry. Output variable *pn is set to the size of the buffer 
** in bytes before returning.
**
** The returned buffer does not include the 0x00 terminator byte stored on
** disk.
*/
const u8 *sqlite3Fts5IterPoslist(Fts5IndexIter *pIter, int *pn){
  Fts5ChunkIter iter;
  Fts5Index *p = pIter->pIndex;
  Fts5SegIter *pSeg = &pIter->pMulti->aSeg[ pIter->pMulti->aFirst[1] ];

  assert( sqlite3Fts5IterEof(pIter)==0 );
  fts5ChunkIterInit(p, pSeg, &iter);
  if( fts5ChunkIterEof(p, &iter)==0 ){
    fts5BufferZero(&pIter->poslist);
    fts5BufferGrow(&p->rc, &pIter->poslist, iter.nRem);
    while( fts5ChunkIterEof(p, &iter)==0 ){
      fts5BufferAppendBlob(&p->rc, &pIter->poslist, iter.n, iter.p);
      fts5ChunkIterNext(p, &iter);
    }
  }
  fts5ChunkIterRelease(&iter);

  if( p->rc ) return 0;
  *pn = pIter->poslist.n;
  return pIter->poslist.p;
}

/*
** Close an iterator opened by an earlier call to sqlite3Fts5IndexQuery().
*/
void sqlite3Fts5IterClose(Fts5IndexIter *pIter){
  if( pIter ){
    fts5MultiIterFree(pIter->pIndex, pIter->pMulti);
    fts5StructureRelease(pIter->pStruct);
    fts5CloseReader(pIter->pIndex);
    fts5BufferFree(&pIter->poslist);
    sqlite3_free(pIter);
  }
}

