
#include <assert.h>

// these globals are safe because sqpyte is single-threaded
i64 lastRowid;
unsigned nVmStep = 0;      /* Number of virtual machine steps */
u8 resetSchemaOnFault = 0; /* Reset schema after an error if positive */
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
unsigned nProgressLimit = 0;/* Invoke xProgress() when nVmStep reaches this */
#endif
int iCompare = 0;          /* Result of last OP_Compare operation */

i64 _sqpyte_get_lastRowid() {
    return lastRowid;
}

int gotoVdbeErrorHalt(Vdbe *p, sqlite3 *db, int pc, int rc) {
  // vdbe_error_halt:
  assert( rc );
  p->rc = rc;
  testcase( sqlite3GlobalConfig.xLog!=0 );
  sqlite3_log(rc, "statement aborts at %d: [%s] %s", 
                   pc, p->zSql, p->zErrMsg);
  sqlite3VdbeHalt(p);
  if( rc==SQLITE_IOERR_NOMEM ) db->mallocFailed = 1;
  rc = SQLITE_ERROR;
  if( resetSchemaOnFault>0 ){
    sqlite3ResetOneSchema(db, resetSchemaOnFault-1);
  }

  // vdbe_return:
  db->lastRowid = lastRowid;
  testcase( nVmStep>0 );
  p->aCounter[SQLITE_STMTSTATUS_VM_STEP] += (int)nVmStep;
  sqlite3VdbeLeave(p);
  return rc;
}

int gotoAbortDueToError(Vdbe *p, sqlite3 *db, int pc, int rc) {
  // abort_due_to_error:
  assert( p->zErrMsg==0 );
  if( db->mallocFailed ) rc = SQLITE_NOMEM;
  if( rc!=SQLITE_IOERR_NOMEM ){
    sqlite3SetString(&p->zErrMsg, db, "%s", sqlite3ErrStr(rc));
  }
  // goto vdbe_error_halt;
  rc = gotoVdbeErrorHalt(p, db, pc, rc);
  return rc;
}

int gotoNoMem(Vdbe *p, sqlite3 *db, int pc) {
  int rc;

  // no_mem:
  db->mallocFailed = 1;
  sqlite3SetString(&p->zErrMsg, db, "out of memory");
  rc = SQLITE_NOMEM;
  // goto vdbe_error_halt;
  rc = gotoVdbeErrorHalt(p, db, pc, rc);
  return rc;
}

int gotoTooBig(Vdbe *p, sqlite3 *db, int pc) {
  int rc;
  // too_big:
  sqlite3SetString(&p->zErrMsg, db, "string or blob too big");
  rc = SQLITE_TOOBIG;
  // goto vdbe_error_halt;
  rc = gotoVdbeErrorHalt(p, db, pc, rc);
  return rc;
}

int gotoAbortDueToInterrupt(Vdbe *p, sqlite3 *db, int pc, int rc) {
  // abort_due_to_interrupt:
  assert( db->u1.isInterrupted );
  rc = SQLITE_INTERRUPT;
  p->rc = rc;
  sqlite3SetString(&p->zErrMsg, db, "%s", sqlite3ErrStr(rc));
  // goto vdbe_error_halt;
  rc = gotoVdbeErrorHalt(p, db, pc, rc);
  return rc;  
}

/* ========================================================================== */



/* Opcode: Transaction P1 P2 P3 P4 P5
**
** Begin a transaction on database P1 if a transaction is not already
** active.
** If P2 is non-zero, then a write-transaction is started, or if a 
** read-transaction is already active, it is upgraded to a write-transaction.
** If P2 is zero, then a read-transaction is started.
**
** P1 is the index of the database file on which the transaction is
** started.  Index 0 is the main database file and index 1 is the
** file used for temporary tables.  Indices of 2 or more are used for
** attached databases.
**
** If a write-transaction is started and the Vdbe.usesStmtJournal flag is
** true (this flag is set if the Vdbe may modify more than one row and may
** throw an ABORT exception), a statement transaction may also be opened.
** More specifically, a statement transaction is opened iff the database
** connection is currently not in autocommit mode, or if there are other
** active statements. A statement transaction allows the changes made by this
** VDBE to be rolled back after an error without having to roll back the
** entire transaction. If no error is encountered, the statement transaction
** will automatically commit when the VDBE halts.
**
** If P5!=0 then this opcode also checks the schema cookie against P3
** and the schema generation counter against P4.
** The cookie changes its value whenever the database schema changes.
** This operation is used to detect when that the cookie has changed
** and that the current process needs to reread the schema.  If the schema
** cookie in P3 differs from the schema cookie in the database header or
** if the schema generation counter in P4 differs from the current
** generation counter, then an SQLITE_SCHEMA error is raised and execution
** halts.  The sqlite3_step() wrapper function might then reprepare the
** statement and rerun it from the beginning.
*/
long impl_OP_Transaction(Vdbe *p, sqlite3 *db, long pc, Op *pOp) {
  int rc;
  Btree *pBt;
  int iMeta;
  int iGen;

  assert( p->bIsReader );
  assert( p->readOnly==0 || pOp->p2==0 );
  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  assert( (p->btreeMask & (((yDbMask)1)<<pOp->p1))!=0 );
  if( pOp->p2 && (db->flags & SQLITE_QueryOnly)!=0 ){
    rc = SQLITE_READONLY;
    // goto abort_due_to_error;
    printf("In impl_OP_Transaction():1: abort_due_to_error.\n");
    rc = gotoAbortDueToError(p, db, (int)pc, rc);
    return (long)rc;
  }
  pBt = db->aDb[pOp->p1].pBt;

  if( pBt ){
    rc = sqlite3BtreeBeginTrans(pBt, pOp->p2);
    if( rc==SQLITE_BUSY ){
      p->pc = (int)pc;
      p->rc = rc = SQLITE_BUSY;
      // Translated: goto vdbe_return;
      db->lastRowid = lastRowid;
      testcase( nVmStep>0 );
      p->aCounter[SQLITE_STMTSTATUS_VM_STEP] += (int)nVmStep;
      sqlite3VdbeLeave(p);
      return (long)rc;
    }
    if( rc!=SQLITE_OK ){
      // goto abort_due_to_error;
      printf("In impl_OP_Transaction():2: abort_due_to_error.\n");
      rc = gotoAbortDueToError(p, db, (int)pc, rc);
      return (long)rc;
    }

    if( pOp->p2 && p->usesStmtJournal 
     && (db->autoCommit==0 || db->nVdbeRead>1) 
    ){
      assert( sqlite3BtreeIsInTrans(pBt) );
      if( p->iStatement==0 ){
        assert( db->nStatement>=0 && db->nSavepoint>=0 );
        db->nStatement++; 
        p->iStatement = db->nSavepoint + db->nStatement;
      }

      rc = sqlite3VtabSavepoint(db, SAVEPOINT_BEGIN, p->iStatement-1);
      if( rc==SQLITE_OK ){
        rc = sqlite3BtreeBeginStmt(pBt, p->iStatement);
      }

      /* Store the current value of the database handles deferred constraint
      ** counter. If the statement transaction needs to be rolled back,
      ** the value of this counter needs to be restored too.  */
      p->nStmtDefCons = db->nDeferredCons;
      p->nStmtDefImmCons = db->nDeferredImmCons;
    }

    /* Gather the schema version number for checking */
    sqlite3BtreeGetMeta(pBt, BTREE_SCHEMA_VERSION, (u32 *)&iMeta);
    iGen = db->aDb[pOp->p1].pSchema->iGeneration;
  }else{
    iGen = iMeta = 0;
  }
  assert( pOp->p5==0 || pOp->p4type==P4_INT32 );
  if( pOp->p5 && (iMeta!=pOp->p3 || iGen!=pOp->p4.i) ){
    sqlite3DbFree(db, p->zErrMsg);
    p->zErrMsg = sqlite3DbStrDup(db, "database schema has changed");
    /* If the schema-cookie from the database file matches the cookie 
    ** stored with the in-memory representation of the schema, do
    ** not reload the schema from the database file.
    **
    ** If virtual-tables are in use, this is not just an optimization.
    ** Often, v-tables store their data in other SQLite tables, which
    ** are queried from within xNext() and other v-table methods using
    ** prepared queries. If such a query is out-of-date, we do not want to
    ** discard the database schema, as the user code implementing the
    ** v-table would have to be ready for the sqlite3_vtab structure itself
    ** to be invalidated whenever sqlite3_step() is called from within 
    ** a v-table method.
    */
    if( db->aDb[pOp->p1].pSchema->schema_cookie!=iMeta ){
      sqlite3ResetOneSchema(db, pOp->p1);
    }
    p->expired = 1;
    rc = SQLITE_SCHEMA;
  }
  return (long)rc;
}

/* Opcode: TableLock P1 P2 P3 P4 *
** Synopsis: iDb=P1 root=P2 write=P3
**
** Obtain a lock on a particular table. This instruction is only used when
** the shared-cache feature is enabled. 
**
** P1 is the index of the database in sqlite3.aDb[] of the database
** on which the lock is acquired.  A readlock is obtained if P3==0 or
** a write lock if P3==1.
**
** P2 contains the root-page of the table to lock.
**
** P4 contains a pointer to the name of the table being locked. This is only
** used to generate an error message if the lock cannot be obtained.
*/
long impl_OP_TableLock(Vdbe *p, sqlite3 *db, long rc, Op *pOp) {
  u8 isWriteLock = (u8)pOp->p3;
  if( isWriteLock || 0==(db->flags&SQLITE_ReadUncommitted) ){
    int p1 = pOp->p1; 
    assert( p1>=0 && p1<db->nDb );
    assert( DbMaskTest(p->btreeMask, p1) );
    assert( isWriteLock==0 || isWriteLock==1 );
    rc = (long)sqlite3BtreeLockTable(db->aDb[p1].pBt, pOp->p2, isWriteLock);
    if( (rc&0xFF)==SQLITE_LOCKED ){
      const char *z = pOp->p4.z;
      sqlite3SetString(&p->zErrMsg, db, "database table is locked: %s", z);
    }
  }
  return rc;
}

/* Opcode: Rewind P1 P2 * * *
**
** The next use of the Rowid or Column or Next instruction for P1 
** will refer to the first entry in the database table or index.
** If the table or index is empty, jump immediately to P2.
** If the table or index is not empty, fall through to the following 
** instruction.
**
** This opcode leaves the cursor configured to move in forward order,
** from the beginning toward the end.  In other words, the cursor is
** configured to use Next, not Prev.
*/
long impl_OP_Rewind(Vdbe *p, sqlite3 *db, long *pc, Op *pOp) {
  int rc;
  VdbeCursor *pC;
  BtCursor *pCrsr;
  int res;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( isSorter(pC)==(pOp->opcode==OP_SorterSort) );
  res = 1;
#ifdef SQLITE_DEBUG
  pC->seekOp = OP_Rewind;
#endif  
  if( isSorter(pC) ){
    rc = sqlite3VdbeSorterRewind(pC, &res);
  }else{
    pCrsr = pC->pCursor;
    assert( pCrsr );
    rc = sqlite3BtreeFirst(pCrsr, &res);
    pC->deferredMoveto = 0;
    pC->cacheStatus = CACHE_STALE;
  }
  pC->nullRow = (u8)res;
  assert( pOp->p2>0 && pOp->p2<p->nOp );
  VdbeBranchTaken(res!=0,2);
  if( res ){
    *pc = (long)pOp->p2 - 1;
  }
  return (long)rc;
}

/* Opcode: Column P1 P2 P3 P4 P5
** Synopsis:  r[P3]=PX
**
** Interpret the data that cursor P1 points to as a structure built using
** the MakeRecord instruction.  (See the MakeRecord opcode for additional
** information about the format of the data.)  Extract the P2-th column
** from this record.  If there are less that (P2+1) 
** values in the record, extract a NULL.
**
** The value extracted is stored in register P3.
**
** If the column contains fewer than P2 fields, then extract a NULL.  Or,
** if the P4 argument is a P4_MEM use the value of the P4 argument as
** the result.
**
** If the OPFLAG_CLEARCACHE bit is set on P5 and P1 is a pseudo-table cursor,
** then the cache of the cursor is reset prior to extracting the column.
** The first OP_Column against a pseudo-table after the value of the content
** register has changed should have this bit set.
**
** If the OPFLAG_LENGTHARG and OPFLAG_TYPEOFARG bits are set on P5 when
** the result is guaranteed to only be used as the argument of a length()
** or typeof() function, respectively.  The loading of large blobs can be
** skipped for length() and all content loading can be skipped for typeof().
*/
long impl_OP_Column(Vdbe *p, sqlite3 *db, long pc, Op *pOp) {
  i64 payloadSize64; /* Number of bytes in the record */
  int p2;            /* column number to retrieve */
  VdbeCursor *pC;    /* The VDBE cursor */
  BtCursor *pCrsr;   /* The BTree cursor */
  u32 *aOffset;      /* aOffset[i] is offset to start of data for i-th column */
  int len;           /* The length of the serialized data for the column */
  int i;             /* Loop counter */
  Mem *pDest;        /* Where to write the extracted value */
  Mem sMem;          /* For storing the record being decoded */
  const u8 *zData;   /* Part of the record being decoded */
  const u8 *zHdr;    /* Next unparsed byte of the header */
  const u8 *zEndHdr; /* Pointer to first byte after the header */
  u32 offset;        /* Offset into the data */
  u32 szField;       /* Number of bytes in the content of a field */
  u32 avail;         /* Number of bytes of available data */
  u32 t;             /* A type code from the record header */
  u16 fx;            /* pDest->flags value */
  Mem *pReg;         /* PseudoTable input register */
  int rc;
  Mem *aMem = p->aMem;
  u8 encoding = ENC(db);     /* The database encoding */

  p2 = pOp->p2;
  assert( pOp->p3>0 && pOp->p3<=(p->nMem-p->nCursor) );
  pDest = &aMem[pOp->p3];
  memAboutToChange(p, pDest);
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( p2<pC->nField );
  aOffset = pC->aOffset;
#ifndef SQLITE_OMIT_VIRTUALTABLE
  assert( pC->pVtabCursor==0 ); /* OP_Column never called on virtual table */
#endif
  pCrsr = pC->pCursor;
  assert( pCrsr!=0 || pC->pseudoTableReg>0 ); /* pCrsr NULL on PseudoTables */
  assert( pCrsr!=0 || pC->nullRow );          /* pC->nullRow on PseudoTables */

  /* If the cursor cache is stale, bring it up-to-date */
  rc = sqlite3VdbeCursorMoveto(pC);
  if( rc ) {
    // goto abort_due_to_error;
    printf("In impl_OP_Column(): abort_due_to_error.\n");
    rc = gotoAbortDueToError(p, db, (int)pc, rc);
    return (long)rc;
  }
  if( pC->cacheStatus!=p->cacheCtr ){
    if( pC->nullRow ){
      if( pCrsr==0 ){
        assert( pC->pseudoTableReg>0 );
        pReg = &aMem[pC->pseudoTableReg];
        assert( pReg->flags & MEM_Blob );
        assert( memIsValid(pReg) );
        pC->payloadSize = pC->szRow = avail = pReg->n;
        pC->aRow = (u8*)pReg->z;
      }else{
        sqlite3VdbeMemSetNull(pDest);
        goto op_column_out;
      }
    }else{
      assert( pCrsr );
      if( pC->isTable==0 ){
        assert( sqlite3BtreeCursorIsValid(pCrsr) );
        VVA_ONLY(rc =) sqlite3BtreeKeySize(pCrsr, &payloadSize64);
        assert( rc==SQLITE_OK ); /* True because of CursorMoveto() call above */
        /* sqlite3BtreeParseCellPtr() uses getVarint32() to extract the
        ** payload size, so it is impossible for payloadSize64 to be
        ** larger than 32 bits. */
        assert( (payloadSize64 & SQLITE_MAX_U32)==(u64)payloadSize64 );
        pC->aRow = sqlite3BtreeKeyFetch(pCrsr, &avail);
        pC->payloadSize = (u32)payloadSize64;
      }else{
        assert( sqlite3BtreeCursorIsValid(pCrsr) );
        VVA_ONLY(rc =) sqlite3BtreeDataSize(pCrsr, &pC->payloadSize);
        assert( rc==SQLITE_OK );   /* DataSize() cannot fail */
        pC->aRow = sqlite3BtreeDataFetch(pCrsr, &avail);
      }
      assert( avail<=65536 );  /* Maximum page size is 64KiB */
      if( pC->payloadSize <= (u32)avail ){
        pC->szRow = pC->payloadSize;
      }else{
        pC->szRow = avail;
      }
      if( pC->payloadSize > (u32)db->aLimit[SQLITE_LIMIT_LENGTH] ){
        // goto too_big;
        printf("In impl_OP_Column(): too_big.\n");
        rc = gotoTooBig(p, db, (int)pc);
        return (long)rc;
      }
    }
    pC->cacheStatus = p->cacheCtr;
    pC->iHdrOffset = getVarint32(pC->aRow, offset);
    pC->nHdrParsed = 0;
    aOffset[0] = offset;

    /* Make sure a corrupt database has not given us an oversize header.
    ** Do this now to avoid an oversize memory allocation.
    **
    ** Type entries can be between 1 and 5 bytes each.  But 4 and 5 byte
    ** types use so much data space that there can only be 4096 and 32 of
    ** them, respectively.  So the maximum header length results from a
    ** 3-byte type for each of the maximum of 32768 columns plus three
    ** extra bytes for the header length itself.  32768*3 + 3 = 98307.
    */
    if( offset > 98307 || offset > pC->payloadSize ){
      rc = SQLITE_CORRUPT_BKPT;
      goto op_column_error;
    }

    if( avail<offset ){
      /* pC->aRow does not have to hold the entire row, but it does at least
      ** need to cover the header of the record.  If pC->aRow does not contain
      ** the complete header, then set it to zero, forcing the header to be
      ** dynamically allocated. */
      pC->aRow = 0;
      pC->szRow = 0;
    }

    /* The following goto is an optimization.  It can be omitted and
    ** everything will still work.  But OP_Column is measurably faster
    ** by skipping the subsequent conditional, which is always true.
    */
    assert( pC->nHdrParsed<=p2 );         /* Conditional skipped */
    goto op_column_read_header;
  }

  /* Make sure at least the first p2+1 entries of the header have been
  ** parsed and valid information is in aOffset[] and pC->aType[].
  */
  if( pC->nHdrParsed<=p2 ){
    /* If there is more header available for parsing in the record, try
    ** to extract additional fields up through the p2+1-th field 
    */
    op_column_read_header:
    if( pC->iHdrOffset<aOffset[0] ){
      /* Make sure zData points to enough of the record to cover the header. */
      if( pC->aRow==0 ){
        memset(&sMem, 0, sizeof(sMem));
        rc = sqlite3VdbeMemFromBtree(pCrsr, 0, aOffset[0], 
                                     !pC->isTable, &sMem);
        if( rc!=SQLITE_OK ){
          goto op_column_error;
        }
        zData = (u8*)sMem.z;
      }else{
        zData = pC->aRow;
      }
  
      /* Fill in pC->aType[i] and aOffset[i] values through the p2-th field. */
      i = pC->nHdrParsed;
      offset = aOffset[i];
      zHdr = zData + pC->iHdrOffset;
      zEndHdr = zData + aOffset[0];
      assert( i<=p2 && zHdr<zEndHdr );
      do{
        if( zHdr[0]<0x80 ){
          t = zHdr[0];
          zHdr++;
        }else{
          zHdr += sqlite3GetVarint32(zHdr, &t);
        }
        pC->aType[i] = t;
        szField = sqlite3VdbeSerialTypeLen(t);
        offset += szField;
        if( offset<szField ){  /* True if offset overflows */
          zHdr = &zEndHdr[1];  /* Forces SQLITE_CORRUPT return below */
          break;
        }
        i++;
        aOffset[i] = offset;
      }while( i<=p2 && zHdr<zEndHdr );
      pC->nHdrParsed = i;
      pC->iHdrOffset = (u32)(zHdr - zData);
      if( pC->aRow==0 ){
        sqlite3VdbeMemRelease(&sMem);
        sMem.flags = MEM_Null;
      }
  
      /* The record is corrupt if any of the following are true:
      ** (1) the bytes of the header extend past the declared header size
      **          (zHdr>zEndHdr)
      ** (2) the entire header was used but not all data was used
      **          (zHdr==zEndHdr && offset!=pC->payloadSize)
      ** (3) the end of the data extends beyond the end of the record.
      **          (offset > pC->payloadSize)
      */
      if( (zHdr>=zEndHdr && (zHdr>zEndHdr || offset!=pC->payloadSize))
       || (offset > pC->payloadSize)
      ){
        rc = SQLITE_CORRUPT_BKPT;
        goto op_column_error;
      }
    }

    /* If after trying to extra new entries from the header, nHdrParsed is
    ** still not up to p2, that means that the record has fewer than p2
    ** columns.  So the result will be either the default value or a NULL.
    */
    if( pC->nHdrParsed<=p2 ){
      if( pOp->p4type==P4_MEM ){
        sqlite3VdbeMemShallowCopy(pDest, pOp->p4.pMem, MEM_Static);
      }else{
        sqlite3VdbeMemSetNull(pDest);
      }
      goto op_column_out;
    }
  }

  /* Extract the content for the p2+1-th column.  Control can only
  ** reach this point if aOffset[p2], aOffset[p2+1], and pC->aType[p2] are
  ** all valid.
  */
  assert( p2<pC->nHdrParsed );
  assert( rc==SQLITE_OK );
  assert( sqlite3VdbeCheckMemInvariants(pDest) );
  if( VdbeMemDynamic(pDest) ) sqlite3VdbeMemSetNull(pDest);
  t = pC->aType[p2];
  if( pC->szRow>=aOffset[p2+1] ){
    /* This is the common case where the desired content fits on the original
    ** page - where the content is not on an overflow page */
    sqlite3VdbeSerialGet(pC->aRow+aOffset[p2], t, pDest);
  }else{
    /* This branch happens only when content is on overflow pages */
    if( ((pOp->p5 & (OPFLAG_LENGTHARG|OPFLAG_TYPEOFARG))!=0
          && ((t>=12 && (t&1)==0) || (pOp->p5 & OPFLAG_TYPEOFARG)!=0))
     || (len = sqlite3VdbeSerialTypeLen(t))==0
    ){
      /* Content is irrelevant for
      **    1. the typeof() function,
      **    2. the length(X) function if X is a blob, and
      **    3. if the content length is zero.
      ** So we might as well use bogus content rather than reading
      ** content from disk.  NULL will work for the value for strings
      ** and blobs and whatever is in the payloadSize64 variable
      ** will work for everything else. */
      sqlite3VdbeSerialGet(t<=13 ? (u8*)&payloadSize64 : 0, t, pDest);
    }else{
      rc = sqlite3VdbeMemFromBtree(pCrsr, aOffset[p2], len, !pC->isTable,
                                   pDest);
      if( rc!=SQLITE_OK ){
        goto op_column_error;
      }
      sqlite3VdbeSerialGet((const u8*)pDest->z, t, pDest);
      pDest->flags &= ~MEM_Ephem;
    }
  }
  pDest->enc = encoding;

op_column_out:
  /* If the column value is an ephemeral string, go ahead and persist
  ** that string in case the cursor moves before the column value is
  ** used.  The following code does the equivalent of Deephemeralize()
  ** but does it faster. */
  if( (pDest->flags & MEM_Ephem)!=0 && pDest->z ){
    fx = pDest->flags & (MEM_Str|MEM_Blob);
    assert( fx!=0 );
    zData = (const u8*)pDest->z;
    len = pDest->n;
    if( sqlite3VdbeMemClearAndResize(pDest, len+2) ) {
      // goto no_mem;
      printf("In impl_OP_Column(): no_mem.\n");
      rc = gotoNoMem(p, db, (int)pc);
      return (long)rc;
    }
    memcpy(pDest->z, zData, len);
    pDest->z[len] = 0;
    pDest->z[len+1] = 0;
    pDest->flags = fx|MEM_Term;
  }
op_column_error:
  UPDATE_MAX_BLOBSIZE(pDest);
  REGISTER_TRACE(pOp->p3, pDest);

  return (long)rc | (((int)pDest->flags) << 16);
}

/* Opcode: ResultRow P1 P2 * * *
** Synopsis:  output=r[P1@P2]
**
** The registers P1 through P1+P2-1 contain a single row of
** results. This opcode causes the sqlite3_step() call to terminate
** with an SQLITE_ROW return code and it sets up the sqlite3_stmt
** structure to provide access to the r(P1)..r(P1+P2-1) values as
** the result row.
*/
long impl_OP_ResultRow(Vdbe *p, sqlite3 *db, long pc, Op *pOp) {
  Mem *pMem;
  int i;
  int rc;

  Mem *aMem = p->aMem;

  assert( p->nResColumn==pOp->p2 );
  assert( pOp->p1>0 );
  assert( pOp->p1+pOp->p2<=(p->nMem-p->nCursor)+1 );

#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
  /* Run the progress counter just before returning.
  */
  if( db->xProgress!=0
   && nVmStep>=nProgressLimit
   && db->xProgress(db->pProgressArg)!=0
  ){
    rc = SQLITE_INTERRUPT;
    // goto vdbe_error_halt;
    rc = gotoVdbeErrorHalt(p, db, (int)pc, rc);
    return (long)rc;
  }
#endif

  /* If this statement has violated immediate foreign key constraints, do
  ** not return the number of rows modified. And do not RELEASE the statement
  ** transaction. It needs to be rolled back.  */
  if( SQLITE_OK!=(rc = sqlite3VdbeCheckFk(p, 0)) ){
    assert( db->flags&SQLITE_CountRows );
    assert( p->usesStmtJournal );
    // break;
    return (long)rc;
  }

  /* If the SQLITE_CountRows flag is set in sqlite3.flags mask, then 
  ** DML statements invoke this opcode to return the number of rows 
  ** modified to the user. This is the only way that a VM that
  ** opens a statement transaction may invoke this opcode.
  **
  ** In case this is such a statement, close any statement transaction
  ** opened by this VM before returning control to the user. This is to
  ** ensure that statement-transactions are always nested, not overlapping.
  ** If the open statement-transaction is not closed here, then the user
  ** may step another VM that opens its own statement transaction. This
  ** may lead to overlapping statement transactions.
  **
  ** The statement transaction is never a top-level transaction.  Hence
  ** the RELEASE call below can never fail.
  */
  assert( p->iStatement==0 || db->flags&SQLITE_CountRows );
  rc = sqlite3VdbeCloseStatement(p, SAVEPOINT_RELEASE);
  if( NEVER(rc!=SQLITE_OK) ){
    // break;
    return (long)rc;
  }

  /* Invalidate all ephemeral cursor row caches */
  p->cacheCtr = (p->cacheCtr + 2)|1;

  /* Make sure the results of the current row are \000 terminated
  ** and have an assigned type.  The results are de-ephemeralized as
  ** a side effect.
  */
  pMem = p->pResultSet = &aMem[pOp->p1];
  for(i=0; i<pOp->p2; i++){
    assert( memIsValid(&pMem[i]) );

    // Translated Deephemeralize(&pMem[i]);
    if( ((&pMem[i])->flags&MEM_Ephem)!=0 && sqlite3VdbeMemMakeWriteable(&pMem[i]) ) {
      // goto no_mem;
      printf("In impl_OP_ResultRow():1: no_mem.\n");
      rc = gotoNoMem(p, db, (int)pc);
      return (long)rc;
    }

    assert( (pMem[i].flags & MEM_Ephem)==0
            || (pMem[i].flags & (MEM_Str|MEM_Blob))==0 );
    sqlite3VdbeMemNulTerminate(&pMem[i]);
    REGISTER_TRACE(pOp->p1+i, &pMem[i]);
  }
  if( db->mallocFailed ) {
    // goto no_mem;
    printf("In impl_OP_ResultRow():2: no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }

  /* Return SQLITE_ROW
  */
  p->pc = (int)pc + 1;
  rc = SQLITE_ROW;
  // Translated: goto vdbe_return;
  db->lastRowid = lastRowid;
  testcase( nVmStep>0 );
  p->aCounter[SQLITE_STMTSTATUS_VM_STEP] += (int)nVmStep;
  sqlite3VdbeLeave(p);
  return (long)rc;
}

/* Opcode: Close P1 * * * *
**
** Close a cursor previously opened as P1.  If P1 is not
** currently open, this instruction is a no-op.
*/
void impl_OP_Close(Vdbe *p, Op *pOp) {
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  sqlite3VdbeFreeCursor(p, p->apCsr[pOp->p1]);
  p->apCsr[pOp->p1] = 0;
}

/* Opcode:  Halt P1 P2 * P4 P5
**
** Exit immediately.  All open cursors, etc are closed
** automatically.
**
** P1 is the result code returned by sqlite3_exec(), sqlite3_reset(),
** or sqlite3_finalize().  For a normal halt, this should be SQLITE_OK (0).
** For errors, it can be some other value.  If P1!=0 then P2 will determine
** whether or not to rollback the current transaction.  Do not rollback
** if P2==OE_Fail. Do the rollback if P2==OE_Rollback.  If P2==OE_Abort,
** then back out all changes that have occurred during this execution of the
** VDBE, but do not rollback the transaction. 
**
** If P4 is not null then it is an error message string.
**
** P5 is a value between 0 and 4, inclusive, that modifies the P4 string.
**
**    0:  (no change)
**    1:  NOT NULL contraint failed: P4
**    2:  UNIQUE constraint failed: P4
**    3:  CHECK constraint failed: P4
**    4:  FOREIGN KEY constraint failed: P4
**
** If P5 is not zero and P4 is NULL, then everything after the ":" is
** omitted.
**
** There is an implied "Halt 0 0 0" instruction inserted at the very end of
** every program.  So a jump past the last instruction of the program
** is the same as executing Halt.
*/
long impl_OP_Halt(Vdbe *p, sqlite3 *db, long *pc, Op *pOp) {
  const char *zType;
  const char *zLogFmt;
  int rc;
  Op *aOp;
  Mem *aMem = p->aMem;

  if( pOp->p1==SQLITE_OK && p->pFrame ){
    /* Halt the sub-program. Return control to the parent frame. */
    VdbeFrame *pFrame = p->pFrame;
    p->pFrame = pFrame->pParent;
    p->nFrame--;
    sqlite3VdbeSetChanges(db, p->nChange);
    *pc = (long)sqlite3VdbeFrameRestore(pFrame);
    lastRowid = db->lastRowid;
    if( pOp->p2==OE_Ignore ){
      /* Instruction pc is the OP_Program that invoked the sub-program 
      ** currently being halted. If the p2 instruction of this OP_Halt
      ** instruction is set to OE_Ignore, then the sub-program is throwing
      ** an IGNORE exception. In this case jump to the address specified
      ** as the p2 of the calling OP_Program.  */
      *pc = (long)p->aOp[(int)*pc].p2-1;
    }
    aOp = p->aOp;
    aMem = p->aMem;
    // break;
    return (long)rc;
  }
  p->rc = pOp->p1;
  p->errorAction = (u8)pOp->p2;
  p->pc = (int)*pc;
  if( p->rc ){
    if( pOp->p5 ){
      static const char * const azType[] = { "NOT NULL", "UNIQUE", "CHECK",
                                             "FOREIGN KEY" };
      assert( pOp->p5>=1 && pOp->p5<=4 );
      testcase( pOp->p5==1 );
      testcase( pOp->p5==2 );
      testcase( pOp->p5==3 );
      testcase( pOp->p5==4 );
      zType = azType[pOp->p5-1];
    }else{
      zType = 0;
    }
    assert( zType!=0 || pOp->p4.z!=0 );
    zLogFmt = "abort at %d in [%s]: %s";
    if( zType && pOp->p4.z ){
      sqlite3SetString(&p->zErrMsg, db, "%s constraint failed: %s", 
                       zType, pOp->p4.z);
    }else if( pOp->p4.z ){
      sqlite3SetString(&p->zErrMsg, db, "%s", pOp->p4.z);
    }else{
      sqlite3SetString(&p->zErrMsg, db, "%s constraint failed", zType);
    }
    sqlite3_log(pOp->p1, zLogFmt, *pc, p->zSql, p->zErrMsg);
  }
  rc = sqlite3VdbeHalt(p);
  assert( rc==SQLITE_BUSY || rc==SQLITE_OK || rc==SQLITE_ERROR );
  if( rc==SQLITE_BUSY ){
    p->rc = rc = SQLITE_BUSY;
  }else{
    assert( rc==SQLITE_OK || (p->rc&0xff)==SQLITE_CONSTRAINT );
    assert( rc==SQLITE_OK || db->nDeferredCons>0 || db->nDeferredImmCons>0 );
    rc = p->rc ? SQLITE_ERROR : SQLITE_DONE;
  }
  // Translated: goto vdbe_return;
  db->lastRowid = lastRowid;
  testcase( nVmStep>0 );
  p->aCounter[SQLITE_STMTSTATUS_VM_STEP] += (int)nVmStep;
  sqlite3VdbeLeave(p);
  return (long)rc;
}

/* Opcode: String8 * P2 * P4 *
** Synopsis: r[P2]='P4'
**
** P4 points to a nul terminated UTF-8 string. This opcode is transformed 
** into an OP_String before it is executed for the first time.  During
** this transformation, the length of string P4 is computed and stored
** as the P1 parameter.
*/

void impl_OP_String(Vdbe *p, sqlite3 *db, Op *pOp); /* forward declaration */

long impl_OP_String8(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_String8: {         /* same as TK_STRING, out2-prerelease */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  u8 encoding = ENC(db);     /* The database encoding */
  pOut = &aMem[pOp->p2];

  assert( pOp->p4.z!=0 );
  pOp->opcode = OP_String;
  pOp->p1 = sqlite3Strlen30(pOp->p4.z);

#ifndef SQLITE_OMIT_UTF16
  if( encoding!=SQLITE_UTF8 ){
    rc = (long)sqlite3VdbeMemSetStr(pOut, pOp->p4.z, -1, SQLITE_UTF8, SQLITE_STATIC);
    if( rc==(long)SQLITE_TOOBIG ) {
      // goto too_big;
      printf("In impl_OP_String8():1: too_big.\n");
      rc = (long)gotoTooBig(p, db, (int)pc);
      return rc;
    }
    if( SQLITE_OK!=sqlite3VdbeChangeEncoding(pOut, encoding) ) {
      // goto no_mem;
      printf("In impl_OP_String8(): no_mem.\n");
      rc = (long)gotoNoMem(p, db, (int)pc);
      return rc;      
    }
    assert( pOut->szMalloc>0 && pOut->zMalloc==pOut->z );
    assert( VdbeMemDynamic(pOut)==0 );
    pOut->szMalloc = 0;
    pOut->flags |= MEM_Static;
    if( pOp->p4type==P4_DYNAMIC ){
      sqlite3DbFree(db, pOp->p4.z);
    }
    pOp->p4type = P4_DYNAMIC;
    pOp->p4.z = pOut->z;
    pOp->p1 = pOut->n;
  }
#endif
  if( pOp->p1>db->aLimit[SQLITE_LIMIT_LENGTH] ){
    // goto too_big;
    printf("In impl_OP_String8():2: too_big.\n");
    rc = (long)gotoTooBig(p, db, (int)pc);
    return rc;
  }
  /* Fall through to the next case, OP_String */
  impl_OP_String(p, db, pOp);

  return rc;
}

/* Opcode: String P1 P2 * P4 *
** Synopsis: r[P2]='P4' (len=P1)
**
** The string value P4 of length P1 (bytes) is stored in register P2.
*/
void impl_OP_String(Vdbe *p, sqlite3 *db, Op *pOp) {
// case OP_String: {          /* out2-prerelease */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  u8 encoding = ENC(db);     /* The database encoding */

  pOut = &aMem[pOp->p2];

  assert( pOp->p4.z!=0 );
  pOut->flags = MEM_Str|MEM_Static|MEM_Term;
  pOut->z = pOp->p4.z;
  pOut->n = pOp->p1;
  pOut->enc = encoding;
  UPDATE_MAX_BLOBSIZE(pOut);
  // break;
}

/* Opcode: Rowid P1 P2 * * *
** Synopsis: r[P2]=rowid
**
** Store in register P2 an integer which is the key of the table entry that
** P1 is currently point to.
**
** P1 can be either an ordinary table or a virtual table.  There used to
** be a separate OP_VRowid opcode for use with virtual tables, but this
** one opcode now works for both table types.
*/
long impl_OP_Rowid(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_Rowid: {                 /* out2-prerelease */
  VdbeCursor *pC;
  i64 v;
  sqlite3_vtab *pVtab;
  const sqlite3_module *pModule;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  pOut = &aMem[pOp->p2];
  pOut->flags = MEM_Int;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->pseudoTableReg==0 || pC->nullRow );
  if( pC->nullRow ){
    pOut->flags = MEM_Null;
    // break;
    return rc;
  }else if( pC->deferredMoveto ){
    v = pC->movetoTarget;
  }
#ifndef SQLITE_OMIT_VIRTUALTABLE
  else if( pC->pVtabCursor ){
    pVtab = pC->pVtabCursor->pVtab;
    pModule = pVtab->pModule;
    assert( pModule->xRowid );
    rc = (long)pModule->xRowid(pC->pVtabCursor, &v);
    sqlite3VtabImportErrmsg(p, pVtab);
  }
#endif /* SQLITE_OMIT_VIRTUALTABLE */
  else{
    assert( pC->pCursor!=0 );
    rc = (long)sqlite3VdbeCursorRestore(pC);
    if( rc ) {
      // goto abort_due_to_error;
      printf("In impl_OP_Rowid(): abort_due_to_error.\n");
      rc = (long)gotoAbortDueToError(p, db, (int)pc, (int)rc);
      return rc;
    }
    if( pC->nullRow ){
      pOut->flags = MEM_Null;
      return rc;
      // break;
    }
    rc = (long)sqlite3BtreeKeySize(pC->pCursor, &v);
    assert( rc==SQLITE_OK );  /* Always so because of CursorRestore() above */    
  }
  pOut->u.i = v;

  // break;
  return rc;
}

/* Opcode: OpenEphemeral P1 P2 * P4 P5
** Synopsis: nColumn=P2
**
** Open a new cursor P1 to a transient table.
** The cursor is always opened read/write even if 
** the main database is read-only.  The ephemeral
** table is deleted automatically when the cursor is closed.
**
** P2 is the number of columns in the ephemeral table.
** The cursor points to a BTree table if P4==0 and to a BTree index
** if P4 is not 0.  If P4 is not NULL, it points to a KeyInfo structure
** that defines the format of keys in the index.
**
** The P5 parameter can be a mask of the BTREE_* flags defined
** in btree.h.  These flags control aspects of the operation of
** the btree.  The BTREE_OMIT_JOURNAL and BTREE_SINGLE flags are
** added automatically.
*/
/* Opcode: OpenAutoindex P1 P2 * P4 *
** Synopsis: nColumn=P2
**
** This opcode works the same as OP_OpenEphemeral.  It has a
** different name to distinguish its use.  Tables created using
** by this opcode will be used for automatically created transient
** indices in joins.
*/
long impl_OP_OpenAutoindex_OpenEphemeral(Vdbe *p, sqlite3 *db, long pc, Op *pOp) {
// case OP_OpenAutoindex: 
// case OP_OpenEphemeral: {
  VdbeCursor *pCx;
  KeyInfo *pKeyInfo;
  int rc;

  static const int vfsFlags = 
      SQLITE_OPEN_READWRITE |
      SQLITE_OPEN_CREATE |
      SQLITE_OPEN_EXCLUSIVE |
      SQLITE_OPEN_DELETEONCLOSE |
      SQLITE_OPEN_TRANSIENT_DB;
  assert( pOp->p1>=0 );
  assert( pOp->p2>=0 );
  pCx = allocateCursor(p, pOp->p1, pOp->p2, -1, 1);
  if( pCx==0 ) {
    // goto no_mem;
    printf("In impl_OP_OpenAutoindex_OpenEphemeral(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;                    
  }
  pCx->nullRow = 1;
  pCx->isEphemeral = 1;
  rc = sqlite3BtreeOpen(db->pVfs, 0, db, &pCx->pBt, 
                        BTREE_OMIT_JOURNAL | BTREE_SINGLE | pOp->p5, vfsFlags);
  if( rc==SQLITE_OK ){
    rc = sqlite3BtreeBeginTrans(pCx->pBt, 1);
  }
  if( rc==SQLITE_OK ){
    /* If a transient index is required, create it by calling
    ** sqlite3BtreeCreateTable() with the BTREE_BLOBKEY flag before
    ** opening it. If a transient table is required, just use the
    ** automatically created table with root-page 1 (an BLOB_INTKEY table).
    */
    if( (pKeyInfo = pOp->p4.pKeyInfo)!=0 ){
      int pgno;
      assert( pOp->p4type==P4_KEYINFO );
      rc = sqlite3BtreeCreateTable(pCx->pBt, &pgno, BTREE_BLOBKEY | pOp->p5); 
      if( rc==SQLITE_OK ){
        assert( pgno==MASTER_ROOT+1 );
        assert( pKeyInfo->db==db );
        assert( pKeyInfo->enc==ENC(db) );
        pCx->pKeyInfo = pKeyInfo;
        rc = sqlite3BtreeCursor(pCx->pBt, pgno, 1, pKeyInfo, pCx->pCursor);
      }
      pCx->isTable = 0;
    }else{
      rc = sqlite3BtreeCursor(pCx->pBt, MASTER_ROOT, 1, 0, pCx->pCursor);
      pCx->isTable = 1;
    }
  }
  pCx->isOrdered = (pOp->p5!=BTREE_UNORDERED);
  // break;
  return (long)rc;
}

/* Opcode: IdxInsert P1 P2 P3 * P5
** Synopsis: key=r[P2]
**
** Register P2 holds an SQL index key made using the
** MakeRecord instructions.  This opcode writes that key
** into the index P1.  Data for the entry is nil.
**
** P3 is a flag that provides a hint to the b-tree layer that this
** insert is likely to be an append.
**
** If P5 has the OPFLAG_NCHANGE bit set, then the change counter is
** incremented by this instruction.  If the OPFLAG_NCHANGE bit is clear,
** then the change counter is unchanged.
**
** If P5 has the OPFLAG_USESEEKRESULT bit set, then the cursor must have
** just done a seek to the spot where the new entry is to be inserted.
** This flag avoids doing an extra seek.
**
** This instruction only works for indices.  The equivalent instruction
** for tables is OP_Insert.
*/
long impl_OP_SorterInsert_IdxInsert(Vdbe *p, sqlite3 *db, Op *pOp) {
// case OP_SorterInsert:       /* in2 */
// case OP_IdxInsert: {        /* in2 */
  VdbeCursor *pC;
  BtCursor *pCrsr;
  int nKey;
  const char *zKey;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn2;
  int rc;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( isSorter(pC)==(pOp->opcode==OP_SorterInsert) );
  pIn2 = &aMem[pOp->p2];
  assert( pIn2->flags & MEM_Blob );
  pCrsr = pC->pCursor;
  if( pOp->p5 & OPFLAG_NCHANGE ) p->nChange++;
  assert( pCrsr!=0 );
  assert( pC->isTable==0 );
  rc = ExpandBlob(pIn2);
  if( rc==SQLITE_OK ){
    if( isSorter(pC) ){
      rc = sqlite3VdbeSorterWrite(pC, pIn2);
    }else{
      nKey = pIn2->n;
      zKey = pIn2->z;
      rc = sqlite3BtreeInsert(pCrsr, zKey, nKey, "", 0, 0, pOp->p3, 
          ((pOp->p5 & OPFLAG_USESEEKRESULT) ? pC->seekResult : 0)
          );
      assert( pC->deferredMoveto==0 );
      pC->cacheStatus = CACHE_STALE;
    }
  }
  // break;
  return (long)rc;
}

/* Opcode: Found P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If P4==0 then register P3 holds a blob constructed by MakeRecord.  If
** P4>0 then register P3 is the first of P4 registers that form an unpacked
** record.
**
** Cursor P1 is on an index btree.  If the record identified by P3 and P4
** is a prefix of any entry in P1 then a jump is made to P2 and
** P1 is left pointing at the matching entry.
**
** This operation leaves the cursor in a state where it can be
** advanced in the forward direction.  The Next instruction will work,
** but not the Prev instruction.
**
** See also: NotFound, NoConflict, NotExists. SeekGe
*/
/* Opcode: NotFound P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If P4==0 then register P3 holds a blob constructed by MakeRecord.  If
** P4>0 then register P3 is the first of P4 registers that form an unpacked
** record.
** 
** Cursor P1 is on an index btree.  If the record identified by P3 and P4
** is not the prefix of any entry in P1 then a jump is made to P2.  If P1 
** does contain an entry whose prefix matches the P3/P4 record then control
** falls through to the next instruction and P1 is left pointing at the
** matching entry.
**
** This operation leaves the cursor in a state where it cannot be
** advanced in either direction.  In other words, the Next and Prev
** opcodes do not work after this operation.
**
** See also: Found, NotExists, NoConflict
*/
/* Opcode: NoConflict P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If P4==0 then register P3 holds a blob constructed by MakeRecord.  If
** P4>0 then register P3 is the first of P4 registers that form an unpacked
** record.
** 
** Cursor P1 is on an index btree.  If the record identified by P3 and P4
** contains any NULL value, jump immediately to P2.  If all terms of the
** record are not-NULL then a check is done to determine if any row in the
** P1 index btree has a matching key prefix.  If there are no matches, jump
** immediately to P2.  If there is a match, fall through and leave the P1
** cursor pointing to the matching row.
**
** This opcode is similar to OP_NotFound with the exceptions that the
** branch is always taken if any part of the search key input is NULL.
**
** This operation leaves the cursor in a state where it cannot be
** advanced in either direction.  In other words, the Next and Prev
** opcodes do not work after this operation.
**
** See also: NotFound, Found, NotExists
*/
long impl_OP_NoConflict_NotFound_Found(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
// case OP_NoConflict:     /* jump, in3 */
// case OP_NotFound:       /* jump, in3 */
// case OP_Found: {        /* jump, in3 */
  int alreadyExists;
  int ii;
  VdbeCursor *pC;
  int res;
  char *pFree;
  UnpackedRecord *pIdxKey;
  UnpackedRecord r;
  char aTempRec[ROUND8(sizeof(UnpackedRecord)) + sizeof(Mem)*4 + 7];

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn3;                 /* 3rd input operand */
  Mem *pOut;                 /* Output operand */

#ifdef SQLITE_TEST
  if( pOp->opcode!=OP_NoConflict ) sqlite3_found_count++;
#endif

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pOp->p4type==P4_INT32 );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
#ifdef SQLITE_DEBUG
  pC->seekOp = pOp->opcode;
#endif  
  pIn3 = &aMem[pOp->p3];
  assert( pC->pCursor!=0 );
  assert( pC->isTable==0 );
  pFree = 0;  /* Not needed.  Only used to suppress a compiler warning. */
  if( pOp->p4.i>0 ){
    r.pKeyInfo = pC->pKeyInfo;
    r.nField = (u16)pOp->p4.i;
    r.aMem = pIn3;
    for(ii=0; ii<r.nField; ii++){
      assert( memIsValid(&r.aMem[ii]) );
      ExpandBlob(&r.aMem[ii]);
#ifdef SQLITE_DEBUG
      if( ii ) REGISTER_TRACE(pOp->p3+ii, &r.aMem[ii]);
#endif
    }
    pIdxKey = &r;
  }else{
    pIdxKey = sqlite3VdbeAllocUnpackedRecord(
        pC->pKeyInfo, aTempRec, sizeof(aTempRec), &pFree
    ); 
    if( pIdxKey==0 ) {
      // goto no_mem;
      printf("In impl_OP_NoConflict_NotFound_Found(): no_mem.\n");
      rc = (long)gotoNoMem(p, db, (int)*pc);
      return rc;                              
    }
    assert( pIn3->flags & MEM_Blob );
    ExpandBlob(pIn3);
    sqlite3VdbeRecordUnpack(pC->pKeyInfo, pIn3->n, pIn3->z, pIdxKey);
  }
  pIdxKey->default_rc = 0;
  if( pOp->opcode==OP_NoConflict ){
    /* For the OP_NoConflict opcode, take the jump if any of the
    ** input fields are NULL, since any key with a NULL will not
    ** conflict */
    for(ii=0; ii<pIdxKey->nField; ii++){
      if( pIdxKey->aMem[ii].flags & MEM_Null ){
        *pc = (long)pOp->p2 - 1;
        VdbeBranchTaken(1,2);
        break;
      }
    }
  }
  rc = (long)sqlite3BtreeMovetoUnpacked(pC->pCursor, pIdxKey, 0, 0, &res);
  if( pOp->p4.i==0 ){
    sqlite3DbFree(db, pFree);
  }
  if( rc!=(long)SQLITE_OK ){
    // break;
    return rc;
  }
  pC->seekResult = res;
  alreadyExists = (res==0);
  pC->nullRow = 1-alreadyExists;
  pC->deferredMoveto = 0;
  pC->cacheStatus = CACHE_STALE;
  if( pOp->opcode==OP_Found ){
    VdbeBranchTaken(alreadyExists!=0,2);
    if( alreadyExists ) *pc = (long)pOp->p2 - 1;
  }else{
    VdbeBranchTaken(alreadyExists==0,2);
    if( !alreadyExists ) *pc = (long)pOp->p2 - 1;
  }
  // break;
  return rc;
}

/* Opcode: RowSetTest P1 P2 P3 P4
** Synopsis: if r[P3] in rowset(P1) goto P2
**
** Register P3 is assumed to hold a 64-bit integer value. If register P1
** contains a RowSet object and that RowSet object contains
** the value held in P3, jump to register P2. Otherwise, insert the
** integer in P3 into the RowSet and continue on to the
** next opcode.
**
** The RowSet object is optimized for the case where successive sets
** of integers, where each set contains no duplicates. Each set
** of values is identified by a unique P4 value. The first set
** must have P4==0, the final set P4=-1.  P4 must be either -1 or
** non-negative.  For non-negative values of P4 only the lower 4
** bits are significant.
**
** This allows optimizations: (a) when P4==0 there is no need to test
** the rowset object for P3, as it is guaranteed not to contain it,
** (b) when P4==-1 there is no need to insert the value, as it will
** never be tested for, and (c) when a value that is part of set X is
** inserted, there is no need to search to see if the same value was
** previously inserted as part of set X (only if it was previously
** inserted as part of some other set).
*/
long impl_OP_RowSetTest(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
// case OP_RowSetTest: {                     /* jump, in1, in3 */
  int iSet;
  int exists;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Mem *pIn3;                 /* 3rd input operand */

  pIn1 = &aMem[pOp->p1];
  pIn3 = &aMem[pOp->p3];
  iSet = pOp->p4.i;
  assert( pIn3->flags&MEM_Int );

  /* If there is anything other than a rowset object in memory cell P1,
  ** delete it now and initialize P1 with an empty rowset
  */
  if( (pIn1->flags & MEM_RowSet)==0 ){
    sqlite3VdbeMemSetRowSet(pIn1);
    if( (pIn1->flags & MEM_RowSet)==0 ) {
      // goto no_mem;
      printf("In impl_OP_RowSetTest(): no_mem.\n");
      rc = (long)gotoNoMem(p, db, (int)*pc);
      return rc;                                    
    }
  }

  assert( pOp->p4type==P4_INT32 );
  assert( iSet==-1 || iSet>=0 );
  if( iSet ){
    exists = sqlite3RowSetTest(pIn1->u.pRowSet, iSet, pIn3->u.i);
    VdbeBranchTaken(exists!=0,2);
    if( exists ){
      *pc = (long)pOp->p2 - 1;
      // break;
      return rc;
    }
  }
  if( iSet>=0 ){
    sqlite3RowSetInsert(pIn1->u.pRowSet, pIn3->u.i);
  }
  // break;
  return rc;
}

/* Opcode: SorterOpen P1 P2 * P4 *
**
** This opcode works like OP_OpenEphemeral except that it opens
** a transient index that is specifically designed to sort large
** tables using an external merge-sort algorithm.
*/
long impl_OP_SorterOpen(Vdbe *p, sqlite3 *db, long pc, Op *pOp) {
// case OP_SorterOpen: {
  VdbeCursor *pCx;
  long rc;

  assert( pOp->p1>=0 );
  assert( pOp->p2>=0 );
  pCx = allocateCursor(p, pOp->p1, pOp->p2, -1, 1);
  if( pCx==0 ) {
    // goto no_mem;
    printf("In impl_OP_SorterOpen(): no_mem.\n");
    rc = (long)gotoNoMem(p, db, (int)pc);
    return rc;                                    
  }
  pCx->pKeyInfo = pOp->p4.pKeyInfo;
  assert( pCx->pKeyInfo->db==db );
  assert( pCx->pKeyInfo->enc==ENC(db) );
  rc = (long)sqlite3VdbeSorterInit(db, pOp->p3, pCx);
  // break;
  return rc;
}

/* Opcode: OpenPseudo P1 P2 P3 * *
** Synopsis: P3 columns in r[P2]
**
** Open a new cursor that points to a fake table that contains a single
** row of data.  The content of that one row is the content of memory
** register P2.  In other words, cursor P1 becomes an alias for the 
** MEM_Blob content contained in register P2.
**
** A pseudo-table created by this opcode is used to hold a single
** row output from the sorter so that the row can be decomposed into
** individual columns using the OP_Column opcode.  The OP_Column opcode
** is the only cursor opcode that works with a pseudo-table.
**
** P3 is the number of fields in the records that will be stored by
** the pseudo-table.
*/
long impl_OP_OpenPseudo(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_OpenPseudo: {
  VdbeCursor *pCx;

  assert( pOp->p1>=0 );
  assert( pOp->p3>=0 );
  pCx = allocateCursor(p, pOp->p1, pOp->p3, -1, 0);
  if( pCx==0 ) {
    // goto no_mem;
    printf("In impl_OP_OpenPseudo(): no_mem.\n");
    rc = (long)gotoNoMem(p, db, (int)pc);
    return rc;                                    
  }
  pCx->nullRow = 1;
  pCx->pseudoTableReg = pOp->p2;
  pCx->isTable = 1;
  assert( pOp->p5==0 );
  // break;
  return rc;
}

/* Opcode: Sort P1 P2 * * *
**
** This opcode does exactly the same thing as OP_Rewind except that
** it increments an undocumented global variable used for testing.
**
** Sorting is accomplished by writing records into a sorting index,
** then rewinding that index and playing it back from beginning to
** end.  We use the OP_Sort opcode instead of OP_Rewind to do the
** rewinding so that the global variable will be incremented and
** regression tests can determine whether or not the optimizer is
** correctly optimizing out sorts.
*/
long impl_OP_SorterSort_Sort(Vdbe *p, sqlite3 *db, long *pc, Op *pOp) {
// case OP_SorterSort:    /* jump */
// case OP_Sort: {        /* jump */
#ifdef SQLITE_TEST
  sqlite3_sort_count++;
  sqlite3_search_count--;
#endif
  p->aCounter[SQLITE_STMTSTATUS_SORT]++;
  /* Fall through into OP_Rewind */
  return impl_OP_Rewind(p, db, pc, pOp);
}

/* Opcode: SorterData P1 P2 P3 * *
** Synopsis: r[P2]=data
**
** Write into register P2 the current sorter data for sorter cursor P1.
** Then clear the column header cache on cursor P3.
**
** This opcode is normally use to move a record out of the sorter and into
** a register that is the source for a pseudo-table cursor created using
** OpenPseudo.  That pseudo-table cursor is the one that is identified by
** parameter P3.  Clearing the P3 column cache as part of this opcode saves
** us from having to issue a separate NullRow instruction to clear that cache.
*/
long impl_OP_SorterData(Vdbe *p, Op *pOp) {
// case OP_SorterData: {
  VdbeCursor *pC;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  int rc;

  pOut = &aMem[pOp->p2];
  pC = p->apCsr[pOp->p1];
  assert( isSorter(pC) );
  rc = sqlite3VdbeSorterRowkey(pC, pOut);
  assert( rc!=SQLITE_OK || (pOut->flags & MEM_Blob) );
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  p->apCsr[pOp->p3]->cacheStatus = CACHE_STALE;  
  // break;
  return (long) rc;
}

/* Trimmed down version of OP_SorterNext. */
long impl_OP_SorterNext(Vdbe *p, sqlite3 *db, long *pc, Op *pOp) {
// case OP_SorterNext: {  /* jump */
  VdbeCursor *pC;
  int res;
  long rc;

  pC = p->apCsr[pOp->p1];
  assert( isSorter(pC) );
  res = 0;
  rc = (long)sqlite3VdbeSorterNext(db, pC, &res);
  // goto next_tail;

// next_tail:
  pC->cacheStatus = CACHE_STALE;
  VdbeBranchTaken(res==0,2);
  if( res==0 ){
    pC->nullRow = 0;
    *pc = pOp->p2 - 1;
    p->aCounter[pOp->p5]++;
#ifdef SQLITE_TEST
    sqlite3_search_count++;
#endif
  }else{
    pC->nullRow = 1;
  }

  // goto check_for_interrupt;
  if( db->u1.isInterrupted ) {
    // goto abort_due_to_interrupt;
    printf("In impl_OP_SorterNext(): abort_due_to_interrupt.\n");
    rc = gotoAbortDueToInterrupt(p, db, (int)*pc, rc);
    return (long)rc;
  }
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
  /* Call the progress callback if it is configured and the required number
  ** of VDBE ops have been executed (either since this invocation of
  ** sqlite3VdbeExec() or since last time the progress callback was called).
  ** If the progress callback returns non-zero, exit the virtual machine with
  ** a return code SQLITE_ABORT.
  */
  if( db->xProgress!=0 && nVmStep>=nProgressLimit ){
    assert( db->nProgressOps!=0 );
    nProgressLimit = nVmStep + db->nProgressOps - (nVmStep%db->nProgressOps);
    if( db->xProgress(db->pProgressArg) ){
      rc = SQLITE_INTERRUPT;
      // goto vdbe_error_halt;
      printf("In impl_OP_SorterNext(): vdbe_error_halt.\n");
      rc = gotoVdbeErrorHalt(p, db, (int)*pc, rc);
      return (long)rc;
    }
  }
#endif
  return (long)rc;
}

/* Opcode: NullRow P1 * * * *
**
** Move the cursor P1 to a null row.  Any OP_Column operations
** that occur while the cursor is on the null row will always
** write a NULL.
*/
void impl_OP_NullRow(Vdbe *p, Op *pOp) {
// case OP_NullRow: {
  VdbeCursor *pC;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  pC->nullRow = 1;
  pC->cacheStatus = CACHE_STALE;
  if( pC->pCursor ){
    sqlite3BtreeClearCursor(pC->pCursor);
  }
  // break;
}

/* Opcode: ReadCookie P1 P2 P3 * *
**
** Read cookie number P3 from database P1 and write it into register P2.
** P3==1 is the schema version.  P3==2 is the database format.
** P3==3 is the recommended pager cache size, and so forth.  P1==0 is
** the main database file and P1==1 is the database file used to store
** temporary tables.
**
** There must be a read-lock on the database (either a transaction
** must be started or there must be an open cursor) before
** executing this instruction.
*/
void impl_OP_ReadCookie(Vdbe *p, sqlite3 *db, Op *pOp) {
// case OP_ReadCookie: {               /* out2-prerelease */
  int iMeta;
  int iDb;
  int iCookie;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  pOut = &aMem[pOp->p2];
  pOut->flags = MEM_Int;

  assert( p->bIsReader );
  iDb = pOp->p1;
  iCookie = pOp->p3;
  assert( pOp->p3<SQLITE_N_BTREE_META );
  assert( iDb>=0 && iDb<db->nDb );
  assert( db->aDb[iDb].pBt!=0 );
  assert( DbMaskTest(p->btreeMask, iDb) );

  sqlite3BtreeGetMeta(db->aDb[iDb].pBt, iCookie, (u32 *)&iMeta);
  pOut->u.i = iMeta;
  // break;
}

/* Opcode: NewRowid P1 P2 P3 * *
** Synopsis: r[P2]=rowid
**
** Get a new integer record number (a.k.a "rowid") used as the key to a table.
** The record number is not previously used as a key in the database
** table that cursor P1 points to.  The new record number is written
** written to register P2.
**
** If P3>0 then P3 is a register in the root frame of this VDBE that holds 
** the largest previously generated record number. No new record numbers are
** allowed to be less than this value. When this value reaches its maximum, 
** an SQLITE_FULL error is generated. The P3 register is updated with the '
** generated record number. This P3 mechanism is used to help implement the
** AUTOINCREMENT feature.
*/
long impl_OP_NewRowid(Vdbe *p, sqlite3 *db, long pc, long rcIn, Op *pOp) {
// case OP_NewRowid: {           /* out2-prerelease */
  i64 v;                 /* The new rowid */
  VdbeCursor *pC;        /* Cursor of table to get the new rowid */
  int res;               /* Result of an sqlite3BtreeLast() */
  int cnt;               /* Counter to limit the number of searches */
  Mem *pMem;             /* Register holding largest rowid for AUTOINCREMENT */
  VdbeFrame *pFrame;     /* Root frame of VDBE */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  pOut = &aMem[pOp->p2];
  int rc = (int)rcIn;

  v = 0;
  res = 0;
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  if( NEVER(pC->pCursor==0) ){
    /* The zero initialization above is all that is needed */
  }else{
    /* The next rowid or record number (different terms for the same
    ** thing) is obtained in a two-step algorithm.
    **
    ** First we attempt to find the largest existing rowid and add one
    ** to that.  But if the largest existing rowid is already the maximum
    ** positive integer, we have to fall through to the second
    ** probabilistic algorithm
    **
    ** The second algorithm is to select a rowid at random and see if
    ** it already exists in the table.  If it does not exist, we have
    ** succeeded.  If the random rowid does exist, we select a new one
    ** and try again, up to 100 times.
    */
    assert( pC->isTable );

#ifdef SQLITE_32BIT_ROWID
#   define MAX_ROWID 0x7fffffff
#else
    /* Some compilers complain about constants of the form 0x7fffffffffffffff.
    ** Others complain about 0x7ffffffffffffffffLL.  The following macro seems
    ** to provide the constant while making all compilers happy.
    */
#   define MAX_ROWID  (i64)( (((u64)0x7fffffff)<<32) | (u64)0xffffffff )
#endif

    if( !pC->useRandomRowid ){
      rc = sqlite3BtreeLast(pC->pCursor, &res);
      if( rc!=SQLITE_OK ){
        // goto abort_due_to_error;
        printf("In impl_OP_NewRowid():1: abort_due_to_error.\n");
        rc = gotoAbortDueToError(p, db, (int)pc, rc);
        return (long)rc;
      }
      if( res ){
        v = 1;   /* IMP: R-61914-48074 */
      }else{
        assert( sqlite3BtreeCursorIsValid(pC->pCursor) );
        rc = sqlite3BtreeKeySize(pC->pCursor, &v);
        assert( rc==SQLITE_OK );   /* Cannot fail following BtreeLast() */
        if( v>=MAX_ROWID ){
          pC->useRandomRowid = 1;
        }else{
          v++;   /* IMP: R-29538-34987 */
        }
      }
    }

#ifndef SQLITE_OMIT_AUTOINCREMENT
    if( pOp->p3 ){
      /* Assert that P3 is a valid memory cell. */
      assert( pOp->p3>0 );
      if( p->pFrame ){
        for(pFrame=p->pFrame; pFrame->pParent; pFrame=pFrame->pParent);
        /* Assert that P3 is a valid memory cell. */
        assert( pOp->p3<=pFrame->nMem );
        pMem = &pFrame->aMem[pOp->p3];
      }else{
        /* Assert that P3 is a valid memory cell. */
        assert( pOp->p3<=(p->nMem-p->nCursor) );
        pMem = &aMem[pOp->p3];
        memAboutToChange(p, pMem);
      }
      assert( memIsValid(pMem) );

      REGISTER_TRACE(pOp->p3, pMem);
      sqlite3VdbeMemIntegerify(pMem);
      assert( (pMem->flags & MEM_Int)!=0 );  /* mem(P3) holds an integer */
      if( pMem->u.i==MAX_ROWID || pC->useRandomRowid ){
        rc = SQLITE_FULL;   /* IMP: R-12275-61338 */
        // goto abort_due_to_error;
        printf("In impl_OP_NewRowid():2: abort_due_to_error.\n");
        rc = gotoAbortDueToError(p, db, (int)pc, rc);
        return (long)rc;
      }
      if( v<pMem->u.i+1 ){
        v = pMem->u.i + 1;
      }
      pMem->u.i = v;
    }
#endif
    if( pC->useRandomRowid ){
      /* IMPLEMENTATION-OF: R-07677-41881 If the largest ROWID is equal to the
      ** largest possible integer (9223372036854775807) then the database
      ** engine starts picking positive candidate ROWIDs at random until
      ** it finds one that is not previously used. */
      assert( pOp->p3==0 );  /* We cannot be in random rowid mode if this is
                             ** an AUTOINCREMENT table. */
      cnt = 0;
      do{
        sqlite3_randomness(sizeof(v), &v);
        v &= (MAX_ROWID>>1); v++;  /* Ensure that v is greater than zero */
      }while(  ((rc = sqlite3BtreeMovetoUnpacked(pC->pCursor, 0, (u64)v,
                                                 0, &res))==SQLITE_OK)
            && (res==0)
            && (++cnt<100));
      if( rc==SQLITE_OK && res==0 ){
        rc = SQLITE_FULL;   /* IMP: R-38219-53002 */
        // goto abort_due_to_error;
        printf("In impl_OP_NewRowid():3: abort_due_to_error.\n");
        rc = gotoAbortDueToError(p, db, (int)pc, rc);
        return (long)rc;        
      }
      assert( v>0 );  /* EV: R-40812-03570 */
    }
    pC->deferredMoveto = 0;
    pC->cacheStatus = CACHE_STALE;
  }
  pOut->u.i = v;
  // break;
  return (long)rc;
}

/* Opcode: Insert P1 P2 P3 P4 P5
** Synopsis: intkey=r[P3] data=r[P2]
**
** Write an entry into the table of cursor P1.  A new entry is
** created if it doesn't already exist or the data for an existing
** entry is overwritten.  The data is the value MEM_Blob stored in register
** number P2. The key is stored in register P3. The key must
** be a MEM_Int.
**
** If the OPFLAG_NCHANGE flag of P5 is set, then the row change count is
** incremented (otherwise not).  If the OPFLAG_LASTROWID flag of P5 is set,
** then rowid is stored for subsequent return by the
** sqlite3_last_insert_rowid() function (otherwise it is unmodified).
**
** If the OPFLAG_USESEEKRESULT flag of P5 is set and if the result of
** the last seek operation (OP_NotExists) was a success, then this
** operation will not attempt to find the appropriate row before doing
** the insert but will instead overwrite the row that the cursor is
** currently pointing to.  Presumably, the prior OP_NotExists opcode
** has already positioned the cursor correctly.  This is an optimization
** that boosts performance by avoiding redundant seeks.
**
** If the OPFLAG_ISUPDATE flag is set, then this opcode is part of an
** UPDATE operation.  Otherwise (if the flag is clear) then this opcode
** is part of an INSERT operation.  The difference is only important to
** the update hook.
**
** Parameter P4 may point to a string containing the table-name, or
** may be NULL. If it is not NULL, then the update-hook 
** (sqlite3.xUpdateCallback) is invoked following a successful insert.
**
** (WARNING/TODO: If P1 is a pseudo-cursor and P2 is dynamically
** allocated, then ownership of P2 is transferred to the pseudo-cursor
** and register P2 becomes ephemeral.  If the cursor is changed, the
** value of register P2 will then change.  Make sure this does not
** cause any problems.)
**
** This instruction only works on tables.  The equivalent instruction
** for indices is OP_IdxInsert.
*/
/* Opcode: InsertInt P1 P2 P3 P4 P5
** Synopsis:  intkey=P3 data=r[P2]
**
** This works exactly like OP_Insert except that the key is the
** integer value P3, not the value of the integer stored in register P3.
*/
long impl_OP_Insert_InsertInt(Vdbe *p, sqlite3 *db, Op *pOp) {
// case OP_Insert: 
// case OP_InsertInt: {
  Mem *pData;       /* MEM cell holding data for the record to be inserted */
  Mem *pKey;        /* MEM cell holding key  for the record */
  i64 iKey;         /* The integer ROWID or key for the record to be inserted */
  VdbeCursor *pC;   /* Cursor to table into which insert is written */
  int nZero;        /* Number of zero-bytes to append */
  int seekResult;   /* Result of prior seek or 0 if no USESEEKRESULT flag */
  const char *zDb;  /* database name - used by the update hook */
  const char *zTbl; /* Table name - used by the opdate hook */
  int op;           /* Opcode for update hook: SQLITE_UPDATE or SQLITE_INSERT */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  int rc;

  pData = &aMem[pOp->p2];
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( memIsValid(pData) );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->pCursor!=0 );
  assert( pC->pseudoTableReg==0 );
  assert( pC->isTable );
  REGISTER_TRACE(pOp->p2, pData);

  if( pOp->opcode==OP_Insert ){
    pKey = &aMem[pOp->p3];
    assert( pKey->flags & MEM_Int );
    assert( memIsValid(pKey) );
    REGISTER_TRACE(pOp->p3, pKey);
    iKey = pKey->u.i;
  }else{
    assert( pOp->opcode==OP_InsertInt );
    iKey = pOp->p3;
  }

  if( pOp->p5 & OPFLAG_NCHANGE ) p->nChange++;
  if( pOp->p5 & OPFLAG_LASTROWID ) db->lastRowid = lastRowid = iKey;
  if( pData->flags & MEM_Null ){
    pData->z = 0;
    pData->n = 0;
  }else{
    assert( pData->flags & (MEM_Blob|MEM_Str) );
  }
  seekResult = ((pOp->p5 & OPFLAG_USESEEKRESULT) ? pC->seekResult : 0);
  if( pData->flags & MEM_Zero ){
    nZero = pData->u.nZero;
  }else{
    nZero = 0;
  }
  rc = sqlite3BtreeInsert(pC->pCursor, 0, iKey,
                          pData->z, pData->n, nZero,
                          (pOp->p5 & OPFLAG_APPEND)!=0, seekResult
  );
  pC->deferredMoveto = 0;
  pC->cacheStatus = CACHE_STALE;

  /* Invoke the update-hook if required. */
  if( rc==SQLITE_OK && db->xUpdateCallback && pOp->p4.z ){
    zDb = db->aDb[pC->iDb].zName;
    zTbl = pOp->p4.z;
    op = ((pOp->p5 & OPFLAG_ISUPDATE) ? SQLITE_UPDATE : SQLITE_INSERT);
    assert( pC->isTable );
    db->xUpdateCallback(db->pUpdateArg, op, zDb, zTbl, iKey);
    assert( pC->iDb>=0 );
  }
  // break;
  return (long)rc;
}

/* Opcode: SetCookie P1 P2 P3 * *
**
** Write the content of register P3 (interpreted as an integer)
** into cookie number P2 of database P1.  P2==1 is the schema version.  
** P2==2 is the database format. P2==3 is the recommended pager cache 
** size, and so forth.  P1==0 is the main database file and P1==1 is the 
** database file used to store temporary tables.
**
** A transaction must be started before executing this opcode.
*/
long impl_OP_SetCookie(Vdbe *p, sqlite3 *db, Op *pOp) {
// case OP_SetCookie: {       /* in3 */
  Db *pDb;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn3;                 /* 3rd input operand */
  long rc;

  assert( pOp->p2<SQLITE_N_BTREE_META );
  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  assert( DbMaskTest(p->btreeMask, pOp->p1) );
  assert( p->readOnly==0 );
  pDb = &db->aDb[pOp->p1];
  assert( pDb->pBt!=0 );
  assert( sqlite3SchemaMutexHeld(db, pOp->p1, 0) );
  pIn3 = &aMem[pOp->p3];
  sqlite3VdbeMemIntegerify(pIn3);
  /* See note about index shifting on OP_ReadCookie */
  rc = (long)sqlite3BtreeUpdateMeta(pDb->pBt, pOp->p2, (int)pIn3->u.i);
  if( pOp->p2==BTREE_SCHEMA_VERSION ){
    /* When the schema cookie changes, record the new cookie internally */
    pDb->pSchema->schema_cookie = (int)pIn3->u.i;
    db->flags |= SQLITE_InternChanges;
  }else if( pOp->p2==BTREE_FILE_FORMAT ){
    /* Record changes in the file format */
    pDb->pSchema->file_format = (u8)pIn3->u.i;
  }
  if( pOp->p1==1 ){
    /* Invalidate all prepared statements whenever the TEMP database
    ** schema is changed.  Ticket #1644 */
    sqlite3ExpirePreparedStatements(db);
    p->expired = 0;
  }
  // break;
  return rc;
}

/* Opcode: ParseSchema P1 * * P4 *
**
** Read and parse all entries from the SQLITE_MASTER table of database P1
** that match the WHERE clause P4. 
**
** This opcode invokes the parser to create a new virtual machine,
** then runs the new virtual machine.  It is thus a re-entrant opcode.
*/
long impl_OP_ParseSchema(Vdbe *p, sqlite3 *db, long pc, long rcIn, Op *pOp) {
// case OP_ParseSchema: {
  int iDb;
  const char *zMaster;
  char *zSql;
  InitData initData;
  int rc = (int)rcIn;

  /* Any prepared statement that invokes this opcode will hold mutexes
  ** on every btree.  This is a prerequisite for invoking 
  ** sqlite3InitCallback().
  */
#ifdef SQLITE_DEBUG
  for(iDb=0; iDb<db->nDb; iDb++){
    assert( iDb==1 || sqlite3BtreeHoldsMutex(db->aDb[iDb].pBt) );
  }
#endif

  iDb = pOp->p1;
  assert( iDb>=0 && iDb<db->nDb );
  assert( DbHasProperty(db, iDb, DB_SchemaLoaded) );
  /* Used to be a conditional */ {
    zMaster = SCHEMA_TABLE(iDb);
    initData.db = db;
    initData.iDb = pOp->p1;
    initData.pzErrMsg = &p->zErrMsg;
    zSql = sqlite3MPrintf(db,
       "SELECT name, rootpage, sql FROM '%q'.%s WHERE %s ORDER BY rowid",
       db->aDb[iDb].zName, zMaster, pOp->p4.z);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      assert( db->init.busy==0 );
      db->init.busy = 1;
      initData.rc = SQLITE_OK;
      assert( !db->mallocFailed );
      rc = sqlite3_exec(db, zSql, sqlite3InitCallback, &initData, 0);
      if( rc==SQLITE_OK ) rc = initData.rc;
      sqlite3DbFree(db, zSql);
      db->init.busy = 0;
    }
  }
  if( rc ) sqlite3ResetAllSchemasOfConnection(db);
  if( rc==SQLITE_NOMEM ){
    // goto no_mem;
    printf("In impl_OP_ParseSchema(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }
  // break;  
  return (long)rc;
}

/* Opcode: RowSetAdd P1 P2 * * *
** Synopsis:  rowset(P1)=r[P2]
**
** Insert the integer value held by register P2 into a boolean index
** held in register P1.
**
** An assertion fails if P2 is not an integer.
*/
long impl_OP_RowSetAdd(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_RowSetAdd: {       /* in1, in2 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Mem *pIn2;

  pIn1 = &aMem[pOp->p1];
  pIn2 = &aMem[pOp->p2];
  assert( (pIn2->flags & MEM_Int)!=0 );
  if( (pIn1->flags & MEM_RowSet)==0 ){
    sqlite3VdbeMemSetRowSet(pIn1);
    if( (pIn1->flags & MEM_RowSet)==0 ) {
      // goto no_mem;
      printf("In impl_OP_RowSetAdd(): no_mem.\n");
      rc = (long)gotoNoMem(p, db, (int)pc);
      return rc;      
    }
  }
  sqlite3RowSetInsert(pIn1->u.pRowSet, pIn2->u.i);
  // break;
  return rc;
}

/* Opcode: RowSetRead P1 P2 P3 * *
** Synopsis:  r[P3]=rowset(P1)
**
** Extract the smallest value from boolean index P1 and put that value into
** register P3.  Or, if boolean index P1 is initially empty, leave P3
** unchanged and jump to instruction P2.
*/
long impl_OP_RowSetRead(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
// case OP_RowSetRead: {       /* jump, in1, out3 */
  i64 val;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  if( (pIn1->flags & MEM_RowSet)==0 
   || sqlite3RowSetNext(pIn1->u.pRowSet, &val)==0
  ){
    /* The boolean index is empty */
    sqlite3VdbeMemSetNull(pIn1);
    *pc = pOp->p2 - 1;
    VdbeBranchTaken(1,2);
  }else{
    /* A value was pulled from the index */
    sqlite3VdbeMemSetInt64(&aMem[pOp->p3], val);
    VdbeBranchTaken(0,2);
  }

  // goto check_for_interrupt;
  if( db->u1.isInterrupted ) {
    // goto abort_due_to_interrupt;
    printf("In impl_OP_RowSetRead(): abort_due_to_interrupt.\n");
    rc = (long)gotoAbortDueToInterrupt(p, db, (int)*pc, rc);
    return rc;
  }
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
  /* Call the progress callback if it is configured and the required number
  ** of VDBE ops have been executed (either since this invocation of
  ** sqlite3VdbeExec() or since last time the progress callback was called).
  ** If the progress callback returns non-zero, exit the virtual machine with
  ** a return code SQLITE_ABORT.
  */
  if( db->xProgress!=0 && nVmStep>=nProgressLimit ){
    assert( db->nProgressOps!=0 );
    nProgressLimit = nVmStep + db->nProgressOps - (nVmStep%db->nProgressOps);
    if( db->xProgress(db->pProgressArg) ){
      rc = SQLITE_INTERRUPT;
      // goto vdbe_error_halt;
      printf("In impl_OP_RowSetRead(): vdbe_error_halt.\n");
      rc = (long)gotoVdbeErrorHalt(p, db, (int)*pc, rc);
      return rc;
    }
  }
#endif
  return rc;
}

/* Opcode: Delete P1 P2 * P4 *
**
** Delete the record at which the P1 cursor is currently pointing.
**
** The cursor will be left pointing at either the next or the previous
** record in the table. If it is left pointing at the next record, then
** the next Next instruction will be a no-op.  Hence it is OK to delete
** a record from within an Next loop.
**
** If the OPFLAG_NCHANGE flag of P2 is set, then the row change count is
** incremented (otherwise not).
**
** P1 must not be pseudo-table.  It has to be a real table with
** multiple rows.
**
** If P4 is not NULL, then it is the name of the table that P1 is
** pointing to.  The update hook will be invoked, if it exists.
** If P4 is not NULL then the P1 cursor must have been positioned
** using OP_NotFound prior to invoking this opcode.
*/
long impl_OP_Delete(Vdbe *p, sqlite3 *db, long pc, Op *pOp) {
// case OP_Delete: {
  VdbeCursor *pC;

  int rc;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->pCursor!=0 );  /* Only valid for real tables, no pseudotables */
  assert( pC->deferredMoveto==0 );

#ifdef SQLITE_DEBUG
  /* The seek operation that positioned the cursor prior to OP_Delete will
  ** have also set the pC->movetoTarget field to the rowid of the row that
  ** is being deleted */
  if( pOp->p4.z && pC->isTable ){
    i64 iKey = 0;
    sqlite3BtreeKeySize(pC->pCursor, &iKey);
    assert( pC->movetoTarget==iKey ); 
  }
#endif

  rc = sqlite3BtreeDelete(pC->pCursor);
  pC->cacheStatus = CACHE_STALE;

  /* Invoke the update-hook if required. */
  if( rc==SQLITE_OK && db->xUpdateCallback && pOp->p4.z && pC->isTable ){
    db->xUpdateCallback(db->pUpdateArg, SQLITE_DELETE,
                        db->aDb[pC->iDb].zName, pOp->p4.z, pC->movetoTarget);
    assert( pC->iDb>=0 );
  }
  if( pOp->p2 & OPFLAG_NCHANGE ) p->nChange++;
  // break;
  return (long)rc;
}

/* Opcode: DropTable P1 * * P4 *
**
** Remove the internal (in-memory) data structures that describe
** the table named P4 in database P1.  This is called after a table
** is dropped in order to keep the internal representation of the
** schema consistent with what is on disk.
*/
void impl_OP_DropTable(sqlite3 *db, Op *pOp) {
// case OP_DropTable: {
  sqlite3UnlinkAndDeleteTable(db, pOp->p1, pOp->p4.z);
  // break;
}

/* Opcode: RowData P1 P2 * * *
** Synopsis: r[P2]=data
**
** Write into register P2 the complete row data for cursor P1.
** There is no interpretation of the data.
** It is just copied onto the P2 register exactly as
** it is found in the database file.
**
** If the P1 cursor must be pointing to a valid row (not a NULL row)
** of a real table, not a pseudo-table.
*/
/* Opcode: RowKey P1 P2 * * *
** Synopsis: r[P2]=key
**
** Write into register P2 the complete row key for cursor P1.
** There is no interpretation of the data.
** The key is copied onto the P2 register exactly as
** it is found in the database file.
**
** If the P1 cursor must be pointing to a valid row (not a NULL row)
** of a real table, not a pseudo-table.
*/

long impl_OP_RowKey_RowData(Vdbe* p, sqlite3 *db, long pc, long rc, Op *pOp) {
  Mem *pOut;                 /* Output operand */
  Mem *aMem = p->aMem;

  VdbeCursor *pC;
  BtCursor *pCrsr;
  u32 n;
  i64 n64;

  pOut = &aMem[pOp->p2];
  memAboutToChange(p, pOut);

  /* Note that RowKey and RowData are really exactly the same instruction */
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( isSorter(pC)==0 );
  assert( pC->isTable || pOp->opcode!=OP_RowData );
  assert( pC->isTable==0 || pOp->opcode==OP_RowData );
  assert( pC!=0 );
  assert( pC->nullRow==0 );
  assert( pC->pseudoTableReg==0 );
  assert( pC->pCursor!=0 );
  pCrsr = pC->pCursor;

  /* The OP_RowKey and OP_RowData opcodes always follow OP_NotExists or
  ** OP_Rewind/Op_Next with no intervening instructions that might invalidate
  ** the cursor.  If this where not the case, on of the following assert()s
  ** would fail.  Should this ever change (because of changes in the code
  ** generator) then the fix would be to insert a call to
  ** sqlite3VdbeCursorMoveto().
  */
  assert( pC->deferredMoveto==0 );
  assert( sqlite3BtreeCursorIsValid(pCrsr) );
#if 0  /* Not required due to the previous to assert() statements */
  rc = sqlite3VdbeCursorMoveto(pC);
  if( rc!=SQLITE_OK ) goto abort_due_to_error;
#endif

  if( pC->isTable==0 ){
    assert( !pC->isTable );
    VVA_ONLY(rc =) sqlite3BtreeKeySize(pCrsr, &n64);
    assert( rc==SQLITE_OK );    /* True because of CursorMoveto() call above */
    if( n64>db->aLimit[SQLITE_LIMIT_LENGTH] ){
      printf("In impl_OP_RowKey_RowData(): too_big.\n");
      rc = gotoTooBig(p, db, (int)pc);
      return (long)rc;
    }
    n = (u32)n64;
  }else{
    VVA_ONLY(rc =) sqlite3BtreeDataSize(pCrsr, &n);
    assert( rc==SQLITE_OK );    /* DataSize() cannot fail */
    if( n>(u32)db->aLimit[SQLITE_LIMIT_LENGTH] ){
      printf("In impl_OP_RowKey_RowData(): too_big.\n");
      rc = gotoTooBig(p, db, (int)pc);
      return (long)rc;
    }
  }
  testcase( n==0 );
  if( sqlite3VdbeMemClearAndResize(pOut, MAX(n,32)) ){
    printf("In impl_OP_RowKey_RowData(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }
  pOut->n = n;
  MemSetTypeFlag(pOut, MEM_Blob);
  if( pC->isTable==0 ){
    rc = sqlite3BtreeKey(pCrsr, 0, n, pOut->z);
  }else{
    rc = sqlite3BtreeData(pCrsr, 0, n, pOut->z);
  }
  pOut->enc = SQLITE_UTF8;  /* In case the blob is ever cast to text */
  UPDATE_MAX_BLOBSIZE(pOut);
  REGISTER_TRACE(pOp->p2, pOut);
  return rc;
}

void impl_OP_Blob(Vdbe *p, sqlite3 *db, Op *pOp) {
  /* out2-prerelease */
  Mem *aMem = p->aMem;
  Mem *pOut = &aMem[pOp->p2];                 /* Output operand */
  u8 encoding = ENC(db);     /* The database encoding */
  assert( pOp->p1 <= SQLITE_MAX_LENGTH );
  sqlite3VdbeMemSetStr(pOut, pOp->p4.z, pOp->p1, 0, 0);
  pOut->enc = encoding;
  UPDATE_MAX_BLOBSIZE(pOut);
}

/* Opcode: Concat P1 P2 P3 * *
** Synopsis: r[P3]=r[P2]+r[P1]
**
** Add the text in register P1 onto the end of the text in
** register P2 and store the result in register P3.
** If either the P1 or P2 text are NULL then store NULL in P3.
**
**   P3 = P2 || P1
**
** It is illegal for P1 and P3 to be the same register. Sometimes,
** if P3 is the same register as P2, the implementation is able
** to avoid a memcpy().
*/
long impl_OP_Concat(Vdbe* p, sqlite3 *db, long pc, long rc, Op *pOp) {
  u8 encoding = ENC(db);     /* The database encoding */
  Mem *pOut;                 /* Output operand */
  Mem *pIn1;
  Mem *pIn2;
  Mem *aMem = p->aMem;
  i64 nByte;

  pIn1 = &aMem[pOp->p1];
  pIn2 = &aMem[pOp->p2];
  pOut = &aMem[pOp->p3];
  assert( pIn1!=pOut );
  if( (pIn1->flags | pIn2->flags) & MEM_Null ){
    sqlite3VdbeMemSetNull(pOut);
    return rc;
  }
  if( ExpandBlob(pIn1) || ExpandBlob(pIn2) ){
    printf("In impl_OP_Concat(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }
  // Stringify(pIn1, encoding);
  if(((pIn1)->flags&(MEM_Str|MEM_Blob))==0 && sqlite3VdbeMemStringify(pIn1,encoding,0)){
    // goto no_mem;
    printf("In impl_OP_Concat(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }
  //Stringify(pIn2, encoding);
  if(((pIn2)->flags&(MEM_Str|MEM_Blob))==0 && sqlite3VdbeMemStringify(pIn2,encoding,0)){
    // goto no_mem;
    printf("In impl_OP_Concat(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }
  nByte = pIn1->n + pIn2->n;
  if( nByte>db->aLimit[SQLITE_LIMIT_LENGTH] ){
    // goto too_big;
    printf("In impl_OP_Concat(): too_big.\n");
    rc = gotoTooBig(p, db, (int)pc);
    return (long)rc;
  }
  if( sqlite3VdbeMemGrow(pOut, (int)nByte+2, pOut==pIn2) ){
    // goto no_mem;
    printf("In impl_OP_Concat(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }
  MemSetTypeFlag(pOut, MEM_Str);
  if( pOut!=pIn2 ){
    memcpy(pOut->z, pIn2->z, pIn2->n);
  }
  memcpy(&pOut->z[pIn2->n], pIn1->z, pIn1->n);
  pOut->z[nByte]=0;
  pOut->z[nByte+1] = 0;
  pOut->flags |= MEM_Term;
  pOut->n = (int)nByte;
  pOut->enc = encoding;
  UPDATE_MAX_BLOBSIZE(pOut);
  return rc;
}

/* Opcode: CreateTable P1 P2 * * *
** Synopsis: r[P2]=root iDb=P1
**
** Allocate a new table in the main database file if P1==0 or in the
** auxiliary database file if P1==1 or in an attached database if
** P1>1.  Write the root page number of the new table into
** register P2
**
** The difference between a table and an index is this:  A table must
** have a 4-byte integer key and can have arbitrary data.  An index
** has an arbitrary key but no data.
**
** See also: CreateIndex
*/
/* Opcode: CreateIndex P1 P2 * * *
** Synopsis: r[P2]=root iDb=P1
**
** Allocate a new index in the main database file if P1==0 or in the
** auxiliary database file if P1==1 or in an attached database if
** P1>1.  Write the root page number of the new table into
** register P2.
**
** See documentation on OP_CreateTable for additional information.
*/
long impl_OP_CreateIndex_CreateTable(Vdbe* p, sqlite3 *db, long rc, Op *pOp) {
  /* out2-prerelease */
  int pgno;
  int flags;
  Db *pDb;
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut = &aMem[pOp->p2];

  pgno = 0;
  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  assert( DbMaskTest(p->btreeMask, pOp->p1) );
  assert( p->readOnly==0 );
  pDb = &db->aDb[pOp->p1];
  assert( pDb->pBt!=0 );
  if( pOp->opcode==OP_CreateTable ){
    /* flags = BTREE_INTKEY; */
    flags = BTREE_INTKEY;
  }else{
    flags = BTREE_BLOBKEY;
  }
  rc = sqlite3BtreeCreateTable(pDb->pBt, &pgno, flags);
  pOut->u.i = pgno;
  return (long)rc;
}

/* Opcode: Clear P1 P2 P3
**
** Delete all contents of the database table or index whose root page
** in the database file is given by P1.  But, unlike Destroy, do not
** remove the table or index from the database file.
**
** The table being clear is in the main database file if P2==0.  If
** P2==1 then the table to be clear is in the auxiliary database file
** that is used to store tables create using CREATE TEMPORARY TABLE.
**
** If the P3 value is non-zero, then the table referred to must be an
** intkey table (an SQL table, not an index). In this case the row change 
** count is incremented by the number of rows in the table being cleared. 
** If P3 is greater than zero, then the value stored in register P3 is
** also incremented by the number of rows in the table being cleared.
**
** See also: Destroy
*/
long impl_OP_Clear(Vdbe* p, sqlite3 *db, long rc, Op *pOp) {
  int nChange;
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
 
  nChange = 0;
  assert( p->readOnly==0 );
  assert( DbMaskTest(p->btreeMask, pOp->p2) );
  rc = sqlite3BtreeClearTable(
      db->aDb[pOp->p2].pBt, pOp->p1, (pOp->p3 ? &nChange : 0)
  );
  if( pOp->p3 ){
    p->nChange += nChange;
    if( pOp->p3>0 ){
      assert( memIsValid(&aMem[pOp->p3]) );
      memAboutToChange(p, &aMem[pOp->p3]);
      aMem[pOp->p3].u.i += nChange;
    }
  }
  return rc;
}




/* ___________________________________________________________________ */
/* translated opcodes */
/* ___________________________________________________________________ */


/* Opcode:  Goto * P2 * * *
**
** An unconditional jump to address P2.
** The next instruction executed will be 
** the one at index P2 from the beginning of
** the program.
**
** The P1 parameter is not actually used by this opcode.  However, it
** is sometimes set to 1 instead of 0 as a hint to the command-line shell
** that this Goto is the bottom of a loop and that the lines from P2 down
** to the current line should be indented for EXPLAIN output.
*/
long impl_OP_Goto(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
  *pc = (long)pOp->p2 - 1;

  /* Opcodes that are used as the bottom of a loop (OP_Next, OP_Prev,
  ** OP_VNext, OP_RowSetNext, or OP_SorterNext) all jump here upon
  ** completion.  Check to see if sqlite3_interrupt() has been called
  ** or if the progress callback needs to be invoked. 
  **
  ** This code uses unstructured "goto" statements and does not look clean.
  ** But that is not due to sloppy coding habits. The code is written this
  ** way for performance, to avoid having to run the interrupt and progress
  ** checks on every opcode.  This helps sqlite3_step() to run about 1.5%
  ** faster according to "valgrind --tool=cachegrind" */

  // check_for_interrupt;
  if( db->u1.isInterrupted ) {
    // goto abort_due_to_interrupt;
    printf("In impl_OP_Goto(): abort_due_to_interrupt.\n");
    rc = (long)gotoAbortDueToInterrupt(p, db, (int)*pc, (int)rc);
    return rc;
  }
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
  /* Call the progress callback if it is configured and the required number
  ** of VDBE ops have been executed (either since this invocation of
  ** sqlite3VdbeExec() or since last time the progress callback was called).
  ** If the progress callback returns non-zero, exit the virtual machine with
  ** a return code SQLITE_ABORT.
  */
  if( db->xProgress!=0 && nVmStep>=nProgressLimit ){
    assert( db->nProgressOps!=0 );
    nProgressLimit = nVmStep + db->nProgressOps - (nVmStep%db->nProgressOps);
    if( db->xProgress(db->pProgressArg) ){
      rc = (long)SQLITE_INTERRUPT;
      // goto vdbe_error_halt;
      printf("In impl_OP_Goto(): vdbe_error_halt.\n");
      rc = (long)gotoVdbeErrorHalt(p, db, (int)*pc, (int)rc);
      return rc;
    }
  }
#endif
  return rc;
}

/* Opcode: OpenRead P1 P2 P3 P4 P5
** Synopsis: root=P2 iDb=P3
**
** Open a read-only cursor for the database table whose root page is
** P2 in a database file.  The database file is determined by P3. 
** P3==0 means the main database, P3==1 means the database used for 
** temporary tables, and P3>1 means used the corresponding attached
** database.  Give the new cursor an identifier of P1.  The P1
** values need not be contiguous but all P1 values should be small integers.
** It is an error for P1 to be negative.
**
** If P5!=0 then use the content of register P2 as the root page, not
** the value of P2 itself.
**
** There will be a read lock on the database whenever there is an
** open cursor.  If the database was unlocked prior to this instruction
** then a read lock is acquired as part of this instruction.  A read
** lock allows other processes to read the database but prohibits
** any other process from modifying the database.  The read lock is
** released when all cursors are closed.  If this instruction attempts
** to get a read lock but fails, the script terminates with an
** SQLITE_BUSY error code.
**
** The P4 value may be either an integer (P4_INT32) or a pointer to
** a KeyInfo structure (P4_KEYINFO). If it is a pointer to a KeyInfo 
** structure, then said structure defines the content and collating 
** sequence of the index being opened. Otherwise, if P4 is an integer 
** value, it is set to the number of columns in the table.
**
** See also OpenWrite.
*/
/* Opcode: OpenWrite P1 P2 P3 P4 P5
** Synopsis: root=P2 iDb=P3
**
** Open a read/write cursor named P1 on the table or index whose root
** page is P2.  Or if P5!=0 use the content of register P2 to find the
** root page.
**
** The P4 value may be either an integer (P4_INT32) or a pointer to
** a KeyInfo structure (P4_KEYINFO). If it is a pointer to a KeyInfo 
** structure, then said structure defines the content and collating 
** sequence of the index being opened. Otherwise, if P4 is an integer 
** value, it is set to the number of columns in the table, or to the
** largest index of any column of the table that is actually used.
**
** This instruction works just like OpenRead except that it opens the cursor
** in read/write mode.  For a given table, there can be one or more read-only
** cursors or a single read/write cursor but not both.
**
** See also OpenRead.
*/
long impl_OP_OpenRead_OpenWrite(Vdbe *p, sqlite3 *db, long pc, Op *pOp) {
  int nField;
  KeyInfo *pKeyInfo;
  int p2;
  int iDb;
  int wrFlag;
  Btree *pX;
  VdbeCursor *pCur;
  Db *pDb;

  Mem *aMem = p->aMem;
  Mem *pIn2;
  int rc;

  assert( (pOp->p5&(OPFLAG_P2ISREG|OPFLAG_BULKCSR))==pOp->p5 );
  assert( pOp->opcode==OP_OpenWrite || pOp->p5==0 );
  assert( p->bIsReader );
  assert( pOp->opcode==OP_OpenRead || pOp->opcode==OP_ReopenIdx
          || p->readOnly==0 );

  if( p->expired ){
    rc = SQLITE_ABORT_ROLLBACK;
    // break;
    return (long)rc;
  }

  nField = 0;
  pKeyInfo = 0;
  p2 = pOp->p2;
  iDb = pOp->p3;
  assert( iDb>=0 && iDb<db->nDb );
  assert( DbMaskTest(p->btreeMask, iDb) );
  pDb = &db->aDb[iDb];
  pX = pDb->pBt;
  assert( pX!=0 );
  if( pOp->opcode==OP_OpenWrite ){
    wrFlag = 1;
    assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
    if( pDb->pSchema->file_format < p->minWriteFileFormat ){
      p->minWriteFileFormat = pDb->pSchema->file_format;
    }
  }else{
    wrFlag = 0;
  }
  if( pOp->p5 & OPFLAG_P2ISREG ){
    assert( p2>0 );
    assert( p2<=(p->nMem-p->nCursor) );
    pIn2 = &aMem[p2];
    assert( memIsValid(pIn2) );
    assert( (pIn2->flags & MEM_Int)!=0 );
    sqlite3VdbeMemIntegerify(pIn2);
    p2 = (int)pIn2->u.i;
    /* The p2 value always comes from a prior OP_CreateTable opcode and
    ** that opcode will always set the p2 value to 2 or more or else fail.
    ** If there were a failure, the prepared statement would have halted
    ** before reaching this instruction. */
    if( NEVER(p2<2) ) {
      rc = SQLITE_CORRUPT_BKPT;
      // goto abort_due_to_error;
      printf("In impl_OP_OpenRead_OpenWrite(): abort_due_to_error.\n");
      rc = gotoAbortDueToError(p, db, (int)pc, rc);
      return (long)rc;
    }
  }
  if( pOp->p4type==P4_KEYINFO ){
    pKeyInfo = pOp->p4.pKeyInfo;
    assert( pKeyInfo->enc==ENC(db) );
    assert( pKeyInfo->db==db );
    nField = pKeyInfo->nField+pKeyInfo->nXField;
  }else if( pOp->p4type==P4_INT32 ){
    nField = pOp->p4.i;
  }
  assert( pOp->p1>=0 );
  assert( nField>=0 );
  testcase( nField==0 );  /* Table with INTEGER PRIMARY KEY and nothing else */
  pCur = allocateCursor(p, pOp->p1, nField, iDb, 1);
  if( pCur==0 ) {
    // goto no_mem;
    printf("In impl_OP_OpenRead_OpenWrite(): no_mem.\n");
    rc = gotoNoMem(p, db, (int)pc);
    return (long)rc;
  }
  pCur->nullRow = 1;
  pCur->isOrdered = 1;
  pCur->pgnoRoot = p2;
  rc = sqlite3BtreeCursor(pX, p2, wrFlag, pKeyInfo, pCur->pCursor);
  pCur->pKeyInfo = pKeyInfo;
  assert( OPFLAG_BULKCSR==BTREE_BULKLOAD );
  sqlite3BtreeCursorHints(pCur->pCursor, (pOp->p5 & OPFLAG_BULKCSR));

  /* Set the VdbeCursor.isTable variable. Previous versions of
  ** SQLite used to check if the root-page flags were sane at this point
  ** and report database corruption if they were not, but this check has
  ** since moved into the btree layer.  */  
  pCur->isTable = pOp->p4type!=P4_KEYINFO;

  return (long)rc;
}

/* Opcode: Next P1 P2 P3 P4 P5
**
** Advance cursor P1 so that it points to the next key/data pair in its
** table or index.  If there are no more key/value pairs then fall through
** to the following instruction.  But if the cursor advance was successful,
** jump immediately to P2.
**
** The Next opcode is only valid following an SeekGT, SeekGE, or
** OP_Rewind opcode used to position the cursor.  Next is not allowed
** to follow SeekLT, SeekLE, or OP_Last.
**
** The P1 cursor must be for a real table, not a pseudo-table.  P1 must have
** been opened prior to this opcode or the program will segfault.
**
** The P3 value is a hint to the btree implementation. If P3==1, that
** means P1 is an SQL index and that this instruction could have been
** omitted if that index had been unique.  P3 is usually 0.  P3 is
** always either 0 or 1.
**
** P4 is always of type P4_ADVANCE. The function pointer points to
** sqlite3BtreeNext().
**
** If P5 is positive and the jump is taken, then event counter
** number P5-1 in the prepared statement is incremented.
**
** See also: Prev, NextIfOpen
*/
/* Opcode: NextIfOpen P1 P2 P3 P4 P5
**
** This opcode works just like Next except that if cursor P1 is not
** open it behaves a no-op.
*/
/* Opcode: Prev P1 P2 P3 P4 P5
**
** Back up cursor P1 so that it points to the previous key/data pair in its
** table or index.  If there is no previous key/value pairs then fall through
** to the following instruction.  But if the cursor backup was successful,
** jump immediately to P2.
**
**
** The Prev opcode is only valid following an SeekLT, SeekLE, or
** OP_Last opcode used to position the cursor.  Prev is not allowed
** to follow SeekGT, SeekGE, or OP_Rewind.
**
** The P1 cursor must be for a real table, not a pseudo-table.  If P1 is
** not open then the behavior is undefined.
**
** The P3 value is a hint to the btree implementation. If P3==1, that
** means P1 is an SQL index and that this instruction could have been
** omitted if that index had been unique.  P3 is usually 0.  P3 is
** always either 0 or 1.
**
** P4 is always of type P4_ADVANCE. The function pointer points to
** sqlite3BtreePrevious().
**
** If P5 is positive and the jump is taken, then event counter
** number P5-1 in the prepared statement is incremented.
*/
/* Opcode: PrevIfOpen P1 P2 P3 P4 P5
**
** This opcode works just like Prev except that if cursor P1 is not
** open it behaves a no-op.
*/
long impl_OP_Next(Vdbe *p, sqlite3 *db, long *pc, Op *pOp) {
  VdbeCursor *pC;
  int res;
  int rc;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pOp->p5<ArraySize(p->aCounter) );
  pC = p->apCsr[pOp->p1];
  res = pOp->p3;
  assert( pC!=0 );
  assert( pC->deferredMoveto==0 );
  assert( pC->pCursor );
  assert( res==0 || (res==1 && pC->isTable==0) );
  testcase( res==1 );
  assert( pOp->opcode!=OP_Next || pOp->p4.xAdvance==sqlite3BtreeNext );
  assert( pOp->opcode!=OP_Prev || pOp->p4.xAdvance==sqlite3BtreePrevious );
  assert( pOp->opcode!=OP_NextIfOpen || pOp->p4.xAdvance==sqlite3BtreeNext );
  assert( pOp->opcode!=OP_PrevIfOpen || pOp->p4.xAdvance==sqlite3BtreePrevious);

  /* The Next opcode is only used after SeekGT, SeekGE, and Rewind.
  ** The Prev opcode is only used after SeekLT, SeekLE, and Last. */
  assert( pOp->opcode!=OP_Next || pOp->opcode!=OP_NextIfOpen
       || pC->seekOp==OP_SeekGT || pC->seekOp==OP_SeekGE
       || pC->seekOp==OP_Rewind || pC->seekOp==OP_Found);
  assert( pOp->opcode!=OP_Prev || pOp->opcode!=OP_PrevIfOpen
       || pC->seekOp==OP_SeekLT || pC->seekOp==OP_SeekLE
       || pC->seekOp==OP_Last );

  rc = pOp->p4.xAdvance(pC->pCursor, &res);
next_tail:
  pC->cacheStatus = CACHE_STALE;
  VdbeBranchTaken(res==0,2);
  if( res==0 ){
    pC->nullRow = 0;
    *pc = (long)pOp->p2 - 1;
    p->aCounter[pOp->p5]++;
#ifdef SQLITE_TEST
    sqlite3_search_count++;
#endif
  }else{
    pC->nullRow = 1;
  }

  // goto check_for_interrupt;
  if( db->u1.isInterrupted ) {
    // goto abort_due_to_interrupt;
    printf("In impl_OP_Next(): abort_due_to_interrupt.\n");
    rc = gotoAbortDueToInterrupt(p, db, (int)*pc, rc);
    return (long)rc;
  }
#ifndef SQLITE_OMIT_PROGRESS_CALLBACK
  /* Call the progress callback if it is configured and the required number
  ** of VDBE ops have been executed (either since this invocation of
  ** sqlite3VdbeExec() or since last time the progress callback was called).
  ** If the progress callback returns non-zero, exit the virtual machine with
  ** a return code SQLITE_ABORT.
  */
  if( db->xProgress!=0 && nVmStep>=nProgressLimit ){
    assert( db->nProgressOps!=0 );
    nProgressLimit = nVmStep + db->nProgressOps - (nVmStep%db->nProgressOps);
    if( db->xProgress(db->pProgressArg) ){
      rc = SQLITE_INTERRUPT;
      // goto vdbe_error_halt;
      printf("In impl_OP_Next(): vdbe_error_halt.\n");
      rc = gotoVdbeErrorHalt(p, db, (int)*pc, rc);
      return (long)rc;
    }
  }
#endif
  return (long)rc;
}


/* Opcode: Lt P1 P2 P3 P4 P5
** Synopsis: if r[P1]<r[P3] goto P2
**
** Compare the values in register P1 and P3.  If reg(P3)<reg(P1) then
** jump to address P2.  
**
** If the SQLITE_JUMPIFNULL bit of P5 is set and either reg(P1) or
** reg(P3) is NULL then take the jump.  If the SQLITE_JUMPIFNULL 
** bit is clear then fall through if either operand is NULL.
**
** The SQLITE_AFF_MASK portion of P5 must be an affinity character -
** SQLITE_AFF_TEXT, SQLITE_AFF_INTEGER, and so forth. An attempt is made 
** to coerce both inputs according to this affinity before the
** comparison is made. If the SQLITE_AFF_MASK is 0x00, then numeric
** affinity is used. Note that the affinity conversions are stored
** back into the input registers P1 and P3.  So this opcode can cause
** persistent changes to registers P1 and P3.
**
** Once any conversions have taken place, and neither value is NULL, 
** the values are compared. If both values are blobs then memcmp() is
** used to determine the results of the comparison.  If both values
** are text, then the appropriate collating function specified in
** P4 is  used to do the comparison.  If P4 is not specified then
** memcmp() is used to compare text string.  If both values are
** numeric, then a numeric comparison is used. If the two values
** are of different types, then numbers are considered less than
** strings and strings are considered less than blobs.
**
** If the SQLITE_STOREP2 bit of P5 is set, then do not jump.  Instead,
** store a boolean result (either 0, or 1, or NULL) in register P2.
**
** If the SQLITE_NULLEQ bit is set in P5, then NULL values are considered
** equal to one another, provided that they do not have their MEM_Cleared
** bit set.
*/
/* Opcode: Ne P1 P2 P3 P4 P5
** Synopsis: if r[P1]!=r[P3] goto P2
**
** This works just like the Lt opcode except that the jump is taken if
** the operands in registers P1 and P3 are not equal.  See the Lt opcode for
** additional information.
**
** If SQLITE_NULLEQ is set in P5 then the result of comparison is always either
** true or false and is never NULL.  If both operands are NULL then the result
** of comparison is false.  If either operand is NULL then the result is true.
** If neither operand is NULL the result is the same as it would be if
** the SQLITE_NULLEQ flag were omitted from P5.
*/
/* Opcode: Eq P1 P2 P3 P4 P5
** Synopsis: if r[P1]==r[P3] goto P2
**
** This works just like the Lt opcode except that the jump is taken if
** the operands in registers P1 and P3 are equal.
** See the Lt opcode for additional information.
**
** If SQLITE_NULLEQ is set in P5 then the result of comparison is always either
** true or false and is never NULL.  If both operands are NULL then the result
** of comparison is true.  If either operand is NULL then the result is false.
** If neither operand is NULL the result is the same as it would be if
** the SQLITE_NULLEQ flag were omitted from P5.
*/
/* Opcode: Le P1 P2 P3 P4 P5
** Synopsis: if r[P1]<=r[P3] goto P2
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is less than or equal to the content of
** register P1.  See the Lt opcode for additional information.
*/
/* Opcode: Gt P1 P2 P3 P4 P5
** Synopsis: if r[P1]>r[P3] goto P2
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is greater than the content of
** register P1.  See the Lt opcode for additional information.
*/
/* Opcode: Ge P1 P2 P3 P4 P5
** Synopsis: if r[P1]>=r[P3] goto P2
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is greater than or equal to the content of
** register P1.  See the Lt opcode for additional information.
*/
long impl_OP_Ne_Eq_Gt_Le_Lt_Ge(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
// case OP_Eq:               /* same as TK_EQ, jump, in1, in3 */
// case OP_Ne:               /* same as TK_NE, jump, in1, in3 */
// case OP_Lt:               /* same as TK_LT, jump, in1, in3 */
// case OP_Le:               /* same as TK_LE, jump, in1, in3 */
// case OP_Gt:               /* same as TK_GT, jump, in1, in3 */
// case OP_Ge: {             /* same as TK_GE, jump, in1, in3 */
  int res;            /* Result of the comparison of pIn1 against pIn3 */
  char affinity;      /* Affinity to use for comparison */
  u16 flags1;         /* Copy of initial value of pIn1->flags */
  u16 flags3;         /* Copy of initial value of pIn3->flags */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Mem *pIn3;                 /* 3rd input operand */
  Mem *pOut;                 /* Output operand */
  u8 encoding = ENC(db);

  pIn1 = &aMem[pOp->p1];
  pIn3 = &aMem[pOp->p3];
  flags1 = pIn1->flags;
  flags3 = pIn3->flags;

  if( (flags1 | flags3)&MEM_Null ){
    /* One or both operands are NULL */
    if( pOp->p5 & SQLITE_NULLEQ ){
      /* If SQLITE_NULLEQ is set (which will only happen if the operator is
      ** OP_Eq or OP_Ne) then take the jump or not depending on whether
      ** or not both operands are null.
      */
      assert( pOp->opcode==OP_Eq || pOp->opcode==OP_Ne );
      assert( (flags1 & MEM_Cleared)==0 );
      assert( (pOp->p5 & SQLITE_JUMPIFNULL)==0 );
      if( (flags1&MEM_Null)!=0
       && (flags3&MEM_Null)!=0
       && (flags3&MEM_Cleared)==0
      ){
        res = 0;  /* Results are equal */
      }else{
        res = 1;  /* Results are not equal */
      }
    }else{
      /* SQLITE_NULLEQ is clear and at least one operand is NULL,
      ** then the result is always NULL.
      ** The jump is taken if the SQLITE_JUMPIFNULL bit is set.
      */
      if( pOp->p5 & SQLITE_STOREP2 ){
        pOut = &aMem[pOp->p2];
        MemSetTypeFlag(pOut, MEM_Null);
        REGISTER_TRACE(pOp->p2, pOut);
      }else{
        VdbeBranchTaken(2,3);
        if( pOp->p5 & SQLITE_JUMPIFNULL ){
          *pc = (long)pOp->p2-1;
        }
      }
      // break;
      return rc;
    }
  }else{
    /* Neither operand is NULL.  Do a comparison. */
    affinity = pOp->p5 & SQLITE_AFF_MASK;
    if( affinity>=SQLITE_AFF_NUMERIC ){
      if( (pIn1->flags & (MEM_Int|MEM_Real|MEM_Str))==MEM_Str ){
        applyNumericAffinity(pIn1,0);
      }
      if( (pIn3->flags & (MEM_Int|MEM_Real|MEM_Str))==MEM_Str ){
        applyNumericAffinity(pIn3,0);
      }
    }else if( affinity==SQLITE_AFF_TEXT ){
      if( (pIn1->flags & MEM_Str)==0 && (pIn1->flags & (MEM_Int|MEM_Real))!=0 ){
        testcase( pIn1->flags & MEM_Int );
        testcase( pIn1->flags & MEM_Real );
        sqlite3VdbeMemStringify(pIn1, encoding, 1);
      }
      if( (pIn3->flags & MEM_Str)==0 && (pIn3->flags & (MEM_Int|MEM_Real))!=0 ){
        testcase( pIn3->flags & MEM_Int );
        testcase( pIn3->flags & MEM_Real );
        sqlite3VdbeMemStringify(pIn3, encoding, 1);
      }
    }
    assert( pOp->p4type==P4_COLLSEQ || pOp->p4.pColl==0 );
    if( pIn1->flags & MEM_Zero ){
      sqlite3VdbeMemExpandBlob(pIn1);
      flags1 &= ~MEM_Zero;
    }
    if( pIn3->flags & MEM_Zero ){
      sqlite3VdbeMemExpandBlob(pIn3);
      flags3 &= ~MEM_Zero;
    }
    if( db->mallocFailed ) {
      // goto no_mem;
      printf("In impl_OP_Ne_Eq_Gt_Le_Lt_Ge(): no_mem.\n");
      rc = (long)gotoNoMem(p, db, (long)*pc);
      return rc;
    }
    res = sqlite3MemCompare(pIn3, pIn1, pOp->p4.pColl);
  }
  switch( pOp->opcode ){
    case OP_Eq:    res = res==0;     break;
    case OP_Ne:    res = res!=0;     break;
    case OP_Lt:    res = res<0;      break;
    case OP_Le:    res = res<=0;     break;
    case OP_Gt:    res = res>0;      break;
    default:       res = res>=0;     break;
  }

  if( pOp->p5 & SQLITE_STOREP2 ){
    pOut = &aMem[pOp->p2];
    memAboutToChange(p, pOut);
    MemSetTypeFlag(pOut, MEM_Int);
    pOut->u.i = res;
    REGISTER_TRACE(pOp->p2, pOut);
  }else{
    VdbeBranchTaken(res!=0, (pOp->p5 & SQLITE_NULLEQ)?2:3);
    if( res ){
      *pc = (long)pOp->p2-1;
    }
  }
  /* Undo any changes made by applyAffinity() to the input registers. */
  pIn1->flags = flags1;
  pIn3->flags = flags3;
  // break;
  return rc;
}

/* Opcode: Integer P1 P2 * * *
** Synopsis: r[P2]=P1
**
** The 32-bit integer value P1 is written into register P2.
*/
void impl_OP_Integer(Vdbe *p, Op *pOp) {
// case OP_Integer: {         /* out2-prerelease */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  pOut = &aMem[pOp->p2];
  pOut->u.i = pOp->p1;
  // break;
}

/* Opcode: Null P1 P2 P3 * *
** Synopsis:  r[P2..P3]=NULL
**
** Write a NULL into registers P2.  If P3 greater than P2, then also write
** NULL into register P3 and every register in between P2 and P3.  If P3
** is less than P2 (typically P3 is zero) then only register P2 is
** set to NULL.
**
** If the P1 value is non-zero, then also set the MEM_Cleared flag so that
** NULL values will not compare equal even if SQLITE_NULLEQ is set on
** OP_Ne or OP_Eq.
*/
void impl_OP_Null(Vdbe *p, Op *pOp) {
// case OP_Null: {           /* out2-prerelease */
  int cnt;
  u16 nullFlag;
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */

  cnt = pOp->p3-pOp->p2;
  assert( pOp->p3<=(p->nMem-p->nCursor) );
  pOut = &aMem[pOp->p2];
  pOut->flags = nullFlag = pOp->p1 ? (MEM_Null|MEM_Cleared) : MEM_Null;
  while( cnt>0 ){
    pOut++;
    memAboutToChange(p, pOut);
    sqlite3VdbeMemSetNull(pOut);
    pOut->flags = nullFlag;
    cnt--;
  }
  // break;
}

/* Opcode: AggStep * P2 P3 P4 P5
** Synopsis: accum=r[P3] step(r[P2@P5])
**
** Execute the step function for an aggregate.  The
** function has P5 arguments.   P4 is a pointer to the FuncDef
** structure that specifies the function.  Use register
** P3 as the accumulator.
**
** The P5 arguments are taken from register P2 and its
** successors.
*/
long impl_OP_AggStep(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_AggStep: {
  int n;
  int i;
  Mem *pMem;
  Mem *pRec;
  Mem t;
  sqlite3_context ctx;
  sqlite3_value **apVal;
  Mem *aMem = p->aMem;       /* Copy of p->aMem */

  n = pOp->p5;
  assert( n>=0 );
  pRec = &aMem[pOp->p2];
  apVal = p->apArg;
  assert( apVal || n==0 );
  for(i=0; i<n; i++, pRec++){
    assert( memIsValid(pRec) );
    apVal[i] = pRec;
    memAboutToChange(p, pRec);
  }
  ctx.pFunc = pOp->p4.pFunc;
  assert( pOp->p3>0 && pOp->p3<=(p->nMem-p->nCursor) );
  ctx.pMem = pMem = &aMem[pOp->p3];
  pMem->n++;
  sqlite3VdbeMemInit(&t, db, MEM_Null);
  ctx.pOut = &t;
  ctx.isError = 0;
  ctx.pVdbe = p;
  ctx.iOp = pc;
  ctx.skipFlag = 0;
  (ctx.pFunc->xStep)(&ctx, n, apVal); /* IMP: R-24505-23230 */
  if( ctx.isError ){
    sqlite3SetString(&p->zErrMsg, db, "%s", sqlite3_value_text(&t));
    rc = (long)ctx.isError;
  }
  if( ctx.skipFlag ){
    assert( pOp[-1].opcode==OP_CollSeq );
    i = pOp[-1].p1;
    if( i ) sqlite3VdbeMemSetInt64(&aMem[i], 1);
  }
  sqlite3VdbeMemRelease(&t);

  // break;
  return rc;
}

/* Opcode: AggFinal P1 P2 * P4 *
** Synopsis: accum=r[P1] N=P2
**
** Execute the finalizer function for an aggregate.  P1 is
** the memory location that is the accumulator for the aggregate.
**
** P2 is the number of arguments that the step function takes and
** P4 is a pointer to the FuncDef for this function.  The P2
** argument is not used by this opcode.  It is only there to disambiguate
** functions that can take varying numbers of arguments.  The
** P4 argument is only needed for the degenerate case where
** the step function was not previously called.
*/
long impl_OP_AggFinal(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_AggFinal: {
  Mem *pMem;
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  u8 encoding = ENC(db);     /* The database encoding */

  assert( pOp->p1>0 && pOp->p1<=(p->nMem-p->nCursor) );
  pMem = &aMem[pOp->p1];
  assert( (pMem->flags & ~(MEM_Null|MEM_Agg))==0 );
  rc = (long)sqlite3VdbeMemFinalize(pMem, pOp->p4.pFunc);
  if( rc ){
    sqlite3SetString(&p->zErrMsg, db, "%s", sqlite3_value_text(pMem));
  }
  sqlite3VdbeChangeEncoding(pMem, encoding);
  UPDATE_MAX_BLOBSIZE(pMem);
  if( sqlite3VdbeMemTooBig(pMem) ){
    // goto too_big;
    printf("In impl_OP_AggFinal(): too_big.\n");
    rc = (long)gotoTooBig(p, db, (int)pc);
    return rc;
  }
  // break;
  return rc;
}

/* Opcode: Copy P1 P2 P3 * *
** Synopsis: r[P2@P3+1]=r[P1@P3+1]
**
** Make a copy of registers P1..P1+P3 into registers P2..P2+P3.
**
** This instruction makes a deep copy of the value.  A duplicate
** is made of any string or blob constant.  See also OP_SCopy.
*/
long impl_OP_Copy(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_Copy: {
  int n;
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Mem *pOut;                 /* Output operand */

  n = pOp->p3;
  pIn1 = &aMem[pOp->p1];
  pOut = &aMem[pOp->p2];
  assert( pOut!=pIn1 );
  while( 1 ){
    sqlite3VdbeMemShallowCopy(pOut, pIn1, MEM_Ephem);
    // Translated Deephemeralize(pOut);
    if( ((pOut)->flags&MEM_Ephem)!=0 && sqlite3VdbeMemMakeWriteable(pOut) ) {
      // goto no_mem;
      printf("In impl_OP_Copy(): no_mem.\n");
      rc = (long)gotoNoMem(p, db, (int)pc);
      return rc;
    }

#ifdef SQLITE_DEBUG
    pOut->pScopyFrom = 0;
#endif
    REGISTER_TRACE(pOp->p2+pOp->p3-n, pOut);
    if( (n--)==0 ) break;
    pOut++;
    pIn1++;
  }
  // break;
  return rc;
}

/* Opcode: MustBeInt P1 P2 * * *
** 
** Force the value in register P1 to be an integer.  If the value
** in P1 is not an integer and cannot be converted into an integer
** without data loss, then jump immediately to P2, or if P2==0
** raise an SQLITE_MISMATCH exception.
*/
long impl_OP_MustBeInt(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
// case OP_MustBeInt: {            /* jump, in1 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  u8 encoding = ENC(db);     /* The database encoding */

  pIn1 = &aMem[pOp->p1];
  if( (pIn1->flags & MEM_Int)==0 ){
    applyAffinity(pIn1, SQLITE_AFF_NUMERIC, encoding);
    VdbeBranchTaken((pIn1->flags&MEM_Int)==0, 2);
    if( (pIn1->flags & MEM_Int)==0 ){
      if( pOp->p2==0 ){
        rc = (long)SQLITE_MISMATCH;
        // goto abort_due_to_error;
        printf("In impl_OP_MustBeInt(): abort_due_to_error.\n");
        rc = (long)gotoAbortDueToError(p, db, (int)*pc, (int)rc);
        return rc;
      }else{
        *pc = (long)pOp->p2 - 1;
        // break;
        return rc;
      }
    }
  }
  MemSetTypeFlag(pIn1, MEM_Int);
  // break;
  return rc;
}

/* Opcode: NotExists P1 P2 P3 * *
** Synopsis: intkey=r[P3]
**
** P1 is the index of a cursor open on an SQL table btree (with integer
** keys).  P3 is an integer rowid.  If P1 does not contain a record with
** rowid P3 then jump immediately to P2.  If P1 does contain a record
** with rowid P3 then leave the cursor pointing at that record and fall
** through to the next instruction.
**
** The OP_NotFound opcode performs the same operation on index btrees
** (with arbitrary multi-value keys).
**
** This opcode leaves the cursor in a state where it cannot be advanced
** in either direction.  In other words, the Next and Prev opcodes will
** not work following this opcode.
**
** See also: Found, NotFound, NoConflict
*/
long impl_OP_NotExists(Vdbe *p, sqlite3 *db, long *pc, Op *pOp) {
// case OP_NotExists: {        /* jump, in3 */
  VdbeCursor *pC;
  BtCursor *pCrsr;
  int res;
  u64 iKey;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn3;                 /* 3rd input operand */
  int rc;

  pIn3 = &aMem[pOp->p3];
  assert( pIn3->flags & MEM_Int );
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
#ifdef SQLITE_DEBUG
  pC->seekOp = 0;
#endif
  assert( pC->isTable );
  assert( pC->pseudoTableReg==0 );
  pCrsr = pC->pCursor;
  assert( pCrsr!=0 );
  res = 0;
  iKey = pIn3->u.i;
  rc = sqlite3BtreeMovetoUnpacked(pCrsr, 0, iKey, 0, &res);
  pC->movetoTarget = iKey;  /* Used by OP_Delete */
  pC->nullRow = 0;
  pC->cacheStatus = CACHE_STALE;
  pC->deferredMoveto = 0;
  VdbeBranchTaken(res!=0,2);
  if( res!=0 ){
    *pc = (long)pOp->p2 - 1;
  }
  pC->seekResult = res;
  // break;
  return (long)rc;
}



/* Opcode: Function P1 P2 P3 P4 P5
** Synopsis: r[P3]=func(r[P2@P5])
**
** Invoke a user function (P4 is a pointer to a Function structure that
** defines the function) with P5 arguments taken from register P2 and
** successors.  The result of the function is stored in register P3.
** Register P3 must not be one of the function inputs.
**
** P1 is a 32-bit bitmask indicating whether or not each argument to the 
** function was determined to be constant at compile time. If the first
** argument was constant then bit 0 of P1 is set. This is used to determine
** whether meta data associated with a user function argument using the
** sqlite3_set_auxdata() API may be safely retained until the next
** invocation of this opcode.
**
** See also: AggStep and AggFinal
*/
long impl_OP_Function(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_Function: {
  int i;
  Mem *pArg;
  sqlite3_context ctx;
  sqlite3_value **apVal;
  int n;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  u8 encoding = ENC(db);     /* The database encoding */

  n = pOp->p5;
  apVal = p->apArg;
  assert( apVal || n==0 );
  assert( pOp->p3>0 && pOp->p3<=(p->nMem-p->nCursor) );
  ctx.pOut = &aMem[pOp->p3];
  memAboutToChange(p, ctx.pOut);

  assert( n==0 || (pOp->p2>0 && pOp->p2+n<=(p->nMem-p->nCursor)+1) );
  assert( pOp->p3<pOp->p2 || pOp->p3>=pOp->p2+n );
  pArg = &aMem[pOp->p2];
  for(i=0; i<n; i++, pArg++){
    assert( memIsValid(pArg) );
    apVal[i] = pArg;
    // Translated Deephemeralize(pArg);
    if( ((pArg)->flags&MEM_Ephem)!=0 && sqlite3VdbeMemMakeWriteable(pArg) ) {
      // goto no_mem;
      printf("In impl_OP_Function():1: no_mem.\n");
      rc = (long)gotoNoMem(p, db, (int)pc);
      return rc;            
    }
    REGISTER_TRACE(pOp->p2+i, pArg);
  }

  assert( pOp->p4type==P4_FUNCDEF );
  ctx.pFunc = pOp->p4.pFunc;
  ctx.iOp = (int)pc;
  ctx.pVdbe = p;
  MemSetTypeFlag(ctx.pOut, MEM_Null);
  ctx.fErrorOrAux = 0;
  db->lastRowid = lastRowid;
  (*ctx.pFunc->xFunc)(&ctx, n, apVal); /* IMP: R-24505-23230 */
  lastRowid = db->lastRowid;  /* Remember rowid changes made by xFunc */

  /* If the function returned an error, throw an exception */
  if( ctx.fErrorOrAux ){
    if( ctx.isError ){
      sqlite3SetString(&p->zErrMsg, db, "%s", sqlite3_value_text(ctx.pOut));
      rc = (long)ctx.isError;
    }
    sqlite3VdbeDeleteAuxData(p, (int)pc, pOp->p1);
  }

  /* Copy the result of the function into register P3 */
  sqlite3VdbeChangeEncoding(ctx.pOut, encoding);
  if( sqlite3VdbeMemTooBig(ctx.pOut) ){
    // goto too_big;
    printf("In impl_OP_Function():2: too_big.\n");
    rc = (long)gotoTooBig(p, db, (int)pc);
    return rc;    
  }

  REGISTER_TRACE(pOp->p3, ctx.pOut);
  UPDATE_MAX_BLOBSIZE(ctx.pOut);
  return (long)rc | (((int)ctx.pOut->flags) << 16);
}

/* Opcode: Real * P2 * P4 *
** Synopsis: r[P2]=P4
**
** P4 is a pointer to a 64-bit floating point value.
** Write that value into register P2.
*/
void impl_OP_Real(Vdbe *p, Op *pOp) {
// case OP_Real: {            /* same as TK_FLOAT, out2-prerelease */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */

  pOut = &aMem[pOp->p2];
  pOut->flags = MEM_Real;
  assert( !sqlite3IsNaN(*pOp->p4.pReal) );
  pOut->u.r = *pOp->p4.pReal;
  // break;
}

/* Opcode: RealAffinity P1 * * * *
**
** If register P1 holds an integer convert it to a real value.
**
** This opcode is used when extracting information from a column that
** has REAL affinity.  Such column values may still be stored as
** integers, for space efficiency, but after extraction we want them
** to have only a real value.
*/
void impl_OP_RealAffinity(Vdbe *p, Op *pOp) {
// case OP_RealAffinity: {                  /* in1 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  if( pIn1->flags & MEM_Int ){
    sqlite3VdbeMemRealify(pIn1);
  }
  // break;
}

/* Opcode: Add P1 P2 P3 * *
** Synopsis:  r[P3]=r[P1]+r[P2]
**
** Add the value in register P1 to the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Multiply P1 P2 P3 * *
** Synopsis:  r[P3]=r[P1]*r[P2]
**
**
** Multiply the value in register P1 by the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Subtract P1 P2 P3 * *
** Synopsis:  r[P3]=r[P2]-r[P1]
**
** Subtract the value in register P1 from the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Divide P1 P2 P3 * *
** Synopsis:  r[P3]=r[P2]/r[P1]
**
** Divide the value in register P1 by the value in register P2
** and store the result in register P3 (P3=P2/P1). If the value in 
** register P1 is zero, then the result is NULL. If either input is 
** NULL, the result is NULL.
*/
/* Opcode: Remainder P1 P2 P3 * *
** Synopsis:  r[P3]=r[P2]%r[P1]
**
** Compute the remainder after integer register P2 is divided by 
** register P1 and store the result in register P3. 
** If the value in register P1 is zero the result is NULL.
** If either operand is NULL, the result is NULL.
*/
void impl_OP_Add_Subtract_Multiply_Divide_Remainder(Vdbe *p, Op *pOp) {
// case OP_Add:                   /* same as TK_PLUS, in1, in2, out3 */
// case OP_Subtract:              /* same as TK_MINUS, in1, in2, out3 */
// case OP_Multiply:              /* same as TK_STAR, in1, in2, out3 */
// case OP_Divide:                /* same as TK_SLASH, in1, in2, out3 */
// case OP_Remainder: {           /* same as TK_REM, in1, in2, out3 */
  char bIntint;   /* Started out as two integer operands */
  u16 flags;      /* Combined MEM_* flags from both inputs */
  u16 type1;      /* Numeric type of left operand */
  u16 type2;      /* Numeric type of right operand */
  i64 iA;         /* Integer value of left operand */
  i64 iB;         /* Integer value of right operand */
  double rA;      /* Real value of left operand */
  double rB;      /* Real value of right operand */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Mem *pIn2;
  Mem *pOut;                 /* Output operand */

  pIn1 = &aMem[pOp->p1];
  type1 = numericType(pIn1);
  pIn2 = &aMem[pOp->p2];
  type2 = numericType(pIn2);
  pOut = &aMem[pOp->p3];
  flags = pIn1->flags | pIn2->flags;
  if( (flags & MEM_Null)!=0 ) goto arithmetic_result_is_null;
  if( (type1 & type2 & MEM_Int)!=0 ){
    iA = pIn1->u.i;
    iB = pIn2->u.i;
    bIntint = 1;
    switch( pOp->opcode ){
      case OP_Add:       if( sqlite3AddInt64(&iB,iA) ) goto fp_math;  break;
      case OP_Subtract:  if( sqlite3SubInt64(&iB,iA) ) goto fp_math;  break;
      case OP_Multiply:  if( sqlite3MulInt64(&iB,iA) ) goto fp_math;  break;
      case OP_Divide: {
        if( iA==0 ) goto arithmetic_result_is_null;
        if( iA==-1 && iB==SMALLEST_INT64 ) goto fp_math;
        iB /= iA;
        break;
      }
      default: {
        if( iA==0 ) goto arithmetic_result_is_null;
        if( iA==-1 ) iA = 1;
        iB %= iA;
        break;
      }
    }
    pOut->u.i = iB;
    MemSetTypeFlag(pOut, MEM_Int);
  }else{
    bIntint = 0;
fp_math:
    rA = sqlite3VdbeRealValue(pIn1);
    rB = sqlite3VdbeRealValue(pIn2);
    switch( pOp->opcode ){
      case OP_Add:         rB += rA;       break;
      case OP_Subtract:    rB -= rA;       break;
      case OP_Multiply:    rB *= rA;       break;
      case OP_Divide: {
        /* (double)0 In case of SQLITE_OMIT_FLOATING_POINT... */
        if( rA==(double)0 ) goto arithmetic_result_is_null;
        rB /= rA;
        break;
      }
      default: {
        iA = (i64)rA;
        iB = (i64)rB;
        if( iA==0 ) goto arithmetic_result_is_null;
        if( iA==-1 ) iA = 1;
        rB = (double)(iB % iA);
        break;
      }
    }
#ifdef SQLITE_OMIT_FLOATING_POINT
    pOut->u.i = rB;
    MemSetTypeFlag(pOut, MEM_Int);
#else
    if( sqlite3IsNaN(rB) ){
      goto arithmetic_result_is_null;
    }
    pOut->u.r = rB;
    MemSetTypeFlag(pOut, MEM_Real);
    if( ((type1|type2)&MEM_Real)==0 && !bIntint ){
      sqlite3VdbeIntegerAffinity(pOut);
    }
#endif
  }
  return;
  // break;

arithmetic_result_is_null:
  sqlite3VdbeMemSetNull(pOut);
  // break;
  return;
}

/* Opcode: If P1 P2 P3 * *
**
** Jump to P2 if the value in register P1 is true.  The value
** is considered true if it is numeric and non-zero.  If the value
** in P1 is NULL then take the jump if P3 is non-zero.
*/
/* Opcode: IfNot P1 P2 P3 * *
**
** Jump to P2 if the value in register P1 is False.  The value
** is considered false if it has a numeric value of zero.  If the value
** in P1 is NULL then take the jump if P3 is zero.
*/
long impl_OP_If_IfNot(Vdbe *p, long pc, Op *pOp) {
// case OP_If:                 /* jump, in1 */
// case OP_IfNot: {            /* jump, in1 */
  int c;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  if( pIn1->flags & MEM_Null ){
    c = pOp->p3;
  }else{
#ifdef SQLITE_OMIT_FLOATING_POINT
    c = sqlite3VdbeIntValue(pIn1)!=0;
#else
    c = sqlite3VdbeRealValue(pIn1)!=0.0;
#endif
    if( pOp->opcode==OP_IfNot ) c = !c;
  }
  VdbeBranchTaken(c!=0, 2);
  if( c ){
    pc = (long)pOp->p2-1;
  }
  // break;
  return pc;
}


/* Opcode: IsNull P1 P2 * * *
** Synopsis:  if r[P1]==NULL goto P2
**
** Jump to P2 if the value in register P1 is NULL.
*/
long impl_OP_IsNull(Vdbe *p, long pc, Op *pOp) {
// case OP_IsNull: {            /* same as TK_ISNULL, jump, in1 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  VdbeBranchTaken( (pIn1->flags & MEM_Null)!=0, 2);
  if( (pIn1->flags & MEM_Null)!=0 ){
    pc = (long)pOp->p2 - 1;
  }
  // break;
  return pc;
}

/* Opcode: SeekGE P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys), 
** use the value in register P3 as the key.  If cursor P1 refers 
** to an SQL index, then P3 is the first in an array of P4 registers 
** that are used as an unpacked index key. 
**
** Reposition cursor P1 so that  it points to the smallest entry that 
** is greater than or equal to the key value. If there are no records 
** greater than or equal to the key and P2 is not zero, then jump to P2.
**
** This opcode leaves the cursor configured to move in forward order,
** from the beginning toward the end.  In other words, the cursor is
** configured to use Next, not Prev.
**
** See also: Found, NotFound, SeekLt, SeekGt, SeekLe
*/
/* Opcode: SeekGT P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys), 
** use the value in register P3 as a key. If cursor P1 refers 
** to an SQL index, then P3 is the first in an array of P4 registers 
** that are used as an unpacked index key. 
**
** Reposition cursor P1 so that  it points to the smallest entry that 
** is greater than the key value. If there are no records greater than 
** the key and P2 is not zero, then jump to P2.
**
** This opcode leaves the cursor configured to move in forward order,
** from the beginning toward the end.  In other words, the cursor is
** configured to use Next, not Prev.
**
** See also: Found, NotFound, SeekLt, SeekGe, SeekLe
*/
/* Opcode: SeekLT P1 P2 P3 P4 * 
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys), 
** use the value in register P3 as a key. If cursor P1 refers 
** to an SQL index, then P3 is the first in an array of P4 registers 
** that are used as an unpacked index key. 
**
** Reposition cursor P1 so that  it points to the largest entry that 
** is less than the key value. If there are no records less than 
** the key and P2 is not zero, then jump to P2.
**
** This opcode leaves the cursor configured to move in reverse order,
** from the end toward the beginning.  In other words, the cursor is
** configured to use Prev, not Next.
**
** See also: Found, NotFound, SeekGt, SeekGe, SeekLe
*/
/* Opcode: SeekLE P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys), 
** use the value in register P3 as a key. If cursor P1 refers 
** to an SQL index, then P3 is the first in an array of P4 registers 
** that are used as an unpacked index key. 
**
** Reposition cursor P1 so that it points to the largest entry that 
** is less than or equal to the key value. If there are no records 
** less than or equal to the key and P2 is not zero, then jump to P2.
**
** This opcode leaves the cursor configured to move in reverse order,
** from the end toward the beginning.  In other words, the cursor is
** configured to use Prev, not Next.
**
** See also: Found, NotFound, SeekGt, SeekGe, SeekLt
*/
long impl_OP_SeekLT_SeekLE_SeekGE_SeekGT(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
// case OP_SeekLT:         /* jump, in3 */
// case OP_SeekLE:         /* jump, in3 */
// case OP_SeekGE:         /* jump, in3 */
// case OP_SeekGT: {       /* jump, in3 */
  int res;
  int oc;
  VdbeCursor *pC;
  UnpackedRecord r;
  int nField;
  i64 iKey;      /* The rowid we are to seek to */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn3;                 /* 3rd input operand */

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pOp->p2!=0 );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->pseudoTableReg==0 );
  assert( OP_SeekLE == OP_SeekLT+1 );
  assert( OP_SeekGE == OP_SeekLT+2 );
  assert( OP_SeekGT == OP_SeekLT+3 );
  assert( pC->isOrdered );
  assert( pC->pCursor!=0 );
  oc = pOp->opcode;
  pC->nullRow = 0;
#ifdef SQLITE_DEBUG
  pC->seekOp = pOp->opcode;
#endif
  if( pC->isTable ){
    /* The input value in P3 might be of any type: integer, real, string,
    ** blob, or NULL.  But it needs to be an integer before we can do
    ** the seek, so convert it. */
    pIn3 = &aMem[pOp->p3];
    if( (pIn3->flags & (MEM_Int|MEM_Real|MEM_Str))==MEM_Str ){
      applyNumericAffinity(pIn3, 0);
    }
    iKey = sqlite3VdbeIntValue(pIn3);

    /* If the P3 value could not be converted into an integer without
    ** loss of information, then special processing is required... */
    if( (pIn3->flags & MEM_Int)==0 ){
      if( (pIn3->flags & MEM_Real)==0 ){
        /* If the P3 value cannot be converted into any kind of a number,
        ** then the seek is not possible, so jump to P2 */
        *pc = (long)pOp->p2 - 1;  VdbeBranchTaken(1,2);
        // break;
        return rc;
      }

      /* If the approximation iKey is larger than the actual real search
      ** term, substitute >= for > and < for <=. e.g. if the search term
      ** is 4.9 and the integer approximation 5:
      **
      **        (x >  4.9)    ->     (x >= 5)
      **        (x <= 4.9)    ->     (x <  5)
      */
      if( pIn3->u.r<(double)iKey ){
        assert( OP_SeekGE==(OP_SeekGT-1) );
        assert( OP_SeekLT==(OP_SeekLE-1) );
        assert( (OP_SeekLE & 0x0001)==(OP_SeekGT & 0x0001) );
        if( (oc & 0x0001)==(OP_SeekGT & 0x0001) ) oc--;
      }

      /* If the approximation iKey is smaller than the actual real search
      ** term, substitute <= for < and > for >=.  */
      else if( pIn3->u.r>(double)iKey ){
        assert( OP_SeekLE==(OP_SeekLT+1) );
        assert( OP_SeekGT==(OP_SeekGE+1) );
        assert( (OP_SeekLT & 0x0001)==(OP_SeekGE & 0x0001) );
        if( (oc & 0x0001)==(OP_SeekLT & 0x0001) ) oc++;
      }
    } 
    rc = (long)sqlite3BtreeMovetoUnpacked(pC->pCursor, 0, (u64)iKey, 0, &res);
    pC->movetoTarget = iKey;  /* Used by OP_Delete */
    if( rc!=(long)SQLITE_OK ){
      // goto abort_due_to_error;
      printf("In impl_OP_SeekLT_SeekLE_SeekGE_SeekGT():1: abort_due_to_error.\n");
      rc = (long)gotoAbortDueToError(p, db, (int)*pc, rc);
      return rc;
    }
  }else{
    nField = pOp->p4.i;
    assert( pOp->p4type==P4_INT32 );
    assert( nField>0 );
    r.pKeyInfo = pC->pKeyInfo;
    r.nField = (u16)nField;

    /* The next line of code computes as follows, only faster:
    **   if( oc==OP_SeekGT || oc==OP_SeekLE ){
    **     r.default_rc = -1;
    **   }else{
    **     r.default_rc = +1;
    **   }
    */
    r.default_rc = ((1 & (oc - OP_SeekLT)) ? -1 : +1);
    assert( oc!=OP_SeekGT || r.default_rc==-1 );
    assert( oc!=OP_SeekLE || r.default_rc==-1 );
    assert( oc!=OP_SeekGE || r.default_rc==+1 );
    assert( oc!=OP_SeekLT || r.default_rc==+1 );

    r.aMem = &aMem[pOp->p3];
#ifdef SQLITE_DEBUG
    { int i; for(i=0; i<r.nField; i++) assert( memIsValid(&r.aMem[i]) ); }
#endif
    ExpandBlob(r.aMem);
    rc = (long)sqlite3BtreeMovetoUnpacked(pC->pCursor, &r, 0, 0, &res);
    if( rc!=(long)SQLITE_OK ){
      // goto abort_due_to_error;
      printf("In impl_OP_SeekLT_SeekLE_SeekGE_SeekGT():2: abort_due_to_error.\n");
      rc = (long)gotoAbortDueToError(p, db, (int)*pc, rc);
      return rc;
    }
  }
  pC->deferredMoveto = 0;
  pC->cacheStatus = CACHE_STALE;
#ifdef SQLITE_TEST
  sqlite3_search_count++;
#endif
  if( oc>=OP_SeekGE ){  assert( oc==OP_SeekGE || oc==OP_SeekGT );
    if( res<0 || (res==0 && oc==OP_SeekGT) ){
      res = 0;
      rc = (long)sqlite3BtreeNext(pC->pCursor, &res);
      if( rc!=(long)SQLITE_OK ) {
        // goto abort_due_to_error;
        printf("In impl_OP_SeekLT_SeekLE_SeekGE_SeekGT():3: abort_due_to_error.\n");
        rc = gotoAbortDueToError(p, db, (int)*pc, rc);
        return rc;
      }
    }else{
      res = 0;
    }
  }else{
    assert( oc==OP_SeekLT || oc==OP_SeekLE );
    if( res>0 || (res==0 && oc==OP_SeekLT) ){
      res = 0;
      rc = (long)sqlite3BtreePrevious(pC->pCursor, &res);
      if( rc!=(long)SQLITE_OK ) {
        // goto abort_due_to_error;
        printf("In impl_OP_SeekLT_SeekLE_SeekGE_SeekGT():4: abort_due_to_error.\n");
        rc = (long)gotoAbortDueToError(p, db, (int)*pc, rc);
        return rc;
      }
    }else{
      /* res might be negative because the table is empty.  Check to
      ** see if this is the case.
      */
      res = sqlite3BtreeEof(pC->pCursor);
    }
  }
  assert( pOp->p2>0 );
  VdbeBranchTaken(res!=0,2);
  if( res ){
    *pc = (long)pOp->p2 - 1;
  }
  // break;
  return rc;
}

/* Opcode: Move P1 P2 P3 * *
** Synopsis:  r[P2@P3]=r[P1@P3]
**
** Move the P3 values in register P1..P1+P3-1 over into
** registers P2..P2+P3-1.  Registers P1..P1+P3-1 are
** left holding a NULL.  It is an error for register ranges
** P1..P1+P3-1 and P2..P2+P3-1 to overlap.  It is an error
** for P3 to be less than 1.
*/
void impl_OP_Move(Vdbe *p, Op *pOp) {
// case OP_Move: {
  int n;           /* Number of registers left to copy */
  int p1;          /* Register to copy from */
  int p2;          /* Register to copy to */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Mem *pOut;                 /* Output operand */

  n = pOp->p3;
  p1 = pOp->p1;
  p2 = pOp->p2;
  assert( n>0 && p1>0 && p2>0 );
  assert( p1+n<=p2 || p2+n<=p1 );

  pIn1 = &aMem[p1];
  pOut = &aMem[p2];
  do{
    assert( pOut<=&aMem[(p->nMem-p->nCursor)] );
    assert( pIn1<=&aMem[(p->nMem-p->nCursor)] );
    assert( memIsValid(pIn1) );
    memAboutToChange(p, pOut);
    sqlite3VdbeMemMove(pOut, pIn1);
#ifdef SQLITE_DEBUG
    if( pOut->pScopyFrom>=&aMem[p1] && pOut->pScopyFrom<&aMem[p1+pOp->p3] ){
      pOut->pScopyFrom += p1 - pOp->p2;
    }
#endif
    REGISTER_TRACE(p2++, pOut);
    pIn1++;
    pOut++;
  }while( --n );
  // break;
}

/* Opcode: IfZero P1 P2 P3 * *
** Synopsis: r[P1]+=P3, if r[P1]==0 goto P2
**
** The register P1 must contain an integer.  Add literal P3 to the
** value in register P1.  If the result is exactly 0, jump to P2. 
**
** It is illegal to use this instruction on a register that does
** not contain an integer.  An assertion fault will result if you try.
*/
long impl_OP_IfZero(Vdbe *p, long pc, Op *pOp) {
// case OP_IfZero: {        /* jump, in1 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags&MEM_Int );
  pIn1->u.i += pOp->p3;
  VdbeBranchTaken(pIn1->u.i==0, 2);
  if( pIn1->u.i==0 ){
     pc = (long)pOp->p2 - 1;
  }
  // break;
  return pc;
}

/* Opcode: IdxRowid P1 P2 * * *
** Synopsis: r[P2]=rowid
**
** Write into register P2 an integer which is the last entry in the record at
** the end of the index key pointed to by cursor P1.  This integer should be
** the rowid of the table entry to which this index entry points.
**
** See also: Rowid, MakeRecord.
*/
long impl_OP_IdxRowid(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_IdxRowid: {              /* out2-prerelease */
  BtCursor *pCrsr;
  VdbeCursor *pC;
  i64 rowid;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */

  pOut = &aMem[pOp->p2];
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  pCrsr = pC->pCursor;
  assert( pCrsr!=0 );
  pOut->flags = MEM_Null;
  assert( pC->isTable==0 );
  assert( pC->deferredMoveto==0 );

  /* sqlite3VbeCursorRestore() can only fail if the record has been deleted
  ** out from under the cursor.  That will never happend for an IdxRowid
  ** opcode, hence the NEVER() arround the check of the return value.
  */
  rc = sqlite3VdbeCursorRestore(pC);
  if( NEVER(rc!=(long)SQLITE_OK) ) {
    // goto abort_due_to_error;
    printf("In impl_OP_IdxRowid():1: abort_due_to_error.\n");
    rc = (long)gotoAbortDueToError(p, db, (int)pc, (int)rc);
    return rc;      
  }

  if( !pC->nullRow ){
    rowid = 0;  /* Not needed.  Only used to silence a warning. */
    rc = (long)sqlite3VdbeIdxRowid(db, pCrsr, &rowid);
    if( rc!=(long)SQLITE_OK ){
      // goto abort_due_to_error;
      printf("In impl_OP_IdxRowid():2: abort_due_to_error.\n");
      rc = (long)gotoAbortDueToError(p, db, (int)pc, (int)rc);
      return rc;      
    }
    pOut->u.i = rowid;
    pOut->flags = MEM_Int;
  }
  // break;
  return rc;
}

/* Opcode: IdxGE P1 P2 P3 P4 P5
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index 
** key that omits the PRIMARY KEY.  Compare this key value against the index 
** that P1 is currently pointing to, ignoring the PRIMARY KEY or ROWID 
** fields at the end.
**
** If the P1 index entry is greater than or equal to the key value
** then jump to P2.  Otherwise fall through to the next instruction.
*/
/* Opcode: IdxGT P1 P2 P3 P4 P5
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index 
** key that omits the PRIMARY KEY.  Compare this key value against the index 
** that P1 is currently pointing to, ignoring the PRIMARY KEY or ROWID 
** fields at the end.
**
** If the P1 index entry is greater than the key value
** then jump to P2.  Otherwise fall through to the next instruction.
*/
/* Opcode: IdxLT P1 P2 P3 P4 P5
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index 
** key that omits the PRIMARY KEY or ROWID.  Compare this key value against
** the index that P1 is currently pointing to, ignoring the PRIMARY KEY or
** ROWID on the P1 index.
**
** If the P1 index entry is less than the key value then jump to P2.
** Otherwise fall through to the next instruction.
*/
/* Opcode: IdxLE P1 P2 P3 P4 P5
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index 
** key that omits the PRIMARY KEY or ROWID.  Compare this key value against
** the index that P1 is currently pointing to, ignoring the PRIMARY KEY or
** ROWID on the P1 index.
**
** If the P1 index entry is less than or equal to the key value then jump
** to P2. Otherwise fall through to the next instruction.
*/
long impl_OP_IdxLE_IdxGT_IdxLT_IdxGE(Vdbe *p, sqlite3 *db, long *pc, Op *pOp) {
// case OP_IdxLE:          /* jump */
// case OP_IdxGT:          /* jump */
// case OP_IdxLT:          /* jump */
// case OP_IdxGE:  {       /* jump */
  VdbeCursor *pC;
  int res;
  UnpackedRecord r;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  int rc;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->isOrdered );
  assert( pC->pCursor!=0);
  assert( pC->deferredMoveto==0 );
  assert( pOp->p5==0 || pOp->p5==1 );
  assert( pOp->p4type==P4_INT32 );
  r.pKeyInfo = pC->pKeyInfo;
  r.nField = (u16)pOp->p4.i;
  if( pOp->opcode<OP_IdxLT ){
    assert( pOp->opcode==OP_IdxLE || pOp->opcode==OP_IdxGT );
    r.default_rc = -1;
  }else{
    assert( pOp->opcode==OP_IdxGE || pOp->opcode==OP_IdxLT );
    r.default_rc = 0;
  }
  r.aMem = &aMem[pOp->p3];
#ifdef SQLITE_DEBUG
  { int i; for(i=0; i<r.nField; i++) assert( memIsValid(&r.aMem[i]) ); }
#endif
  res = 0;  /* Not needed.  Only used to silence a warning. */
  rc = sqlite3VdbeIdxKeyCompare(db, pC, &r, &res);
  assert( (OP_IdxLE&1)==(OP_IdxLT&1) && (OP_IdxGE&1)==(OP_IdxGT&1) );
  if( (pOp->opcode&1)==(OP_IdxLT&1) ){
    assert( pOp->opcode==OP_IdxLE || pOp->opcode==OP_IdxLT );
    res = -res;
  }else{
    assert( pOp->opcode==OP_IdxGE || pOp->opcode==OP_IdxGT );
    res++;
  }
  VdbeBranchTaken(res>0,2);
  if( res>0 ){
    *pc = (long)pOp->p2 - 1 ;
  }
  // break;
  return (long)rc;
}

/* Opcode: Seek P1 P2 * * *
** Synopsis:  intkey=r[P2]
**
** P1 is an open table cursor and P2 is a rowid integer.  Arrange
** for P1 to move so that it points to the rowid given by P2.
**
** This is actually a deferred seek.  Nothing actually happens until
** the cursor is used to read a record.  That way, if no reads
** occur, no unnecessary I/O happens.
*/
void impl_OP_Seek(Vdbe *p, Op *pOp) {
// case OP_Seek: {    /* in2 */
  VdbeCursor *pC;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn2;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->pCursor!=0 );
  assert( pC->isTable );
  pC->nullRow = 0;
  pIn2 = &aMem[pOp->p2];
  pC->movetoTarget = sqlite3VdbeIntValue(pIn2);
  pC->deferredMoveto = 1;
  // break;
}

/* Opcode: Once P1 P2 * * *
**
** Check if OP_Once flag P1 is set. If so, jump to instruction P2. Otherwise,
** set the flag and fall through to the next instruction.  In other words,
** this opcode causes all following opcodes up through P2 (but not including
** P2) to run just once and to be skipped on subsequent times through the loop.
*/
long impl_OP_Once(Vdbe *p, long pc, Op *pOp) {
// case OP_Once: {             /* jump */
  assert( pOp->p1<p->nOnceFlag );
  VdbeBranchTaken(p->aOnceFlag[pOp->p1]!=0, 2);
  if( p->aOnceFlag[pOp->p1] ){
    pc = (long)pOp->p2-1;
  }else{
    p->aOnceFlag[pOp->p1] = 1;
  }
  // break;
  return pc;
}

/* Opcode: SCopy P1 P2 * * *
** Synopsis: r[P2]=r[P1]
**
** Make a shallow copy of register P1 into register P2.
**
** This instruction makes a shallow copy of the value.  If the value
** is a string or blob, then the copy is only a pointer to the
** original and hence if the original changes so will the copy.
** Worse, if the original is deallocated, the copy becomes invalid.
** Thus the program must guarantee that the original will not change
** during the lifetime of the copy.  Use OP_Copy to make a complete
** copy.
*/
void impl_OP_SCopy(Vdbe *p, Op *pOp) {
// case OP_SCopy: {            /* out2 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Mem *pOut;                 /* Output operand */

  pIn1 = &aMem[pOp->p1];
  pOut = &aMem[pOp->p2];
  assert( pOut!=pIn1 );
  sqlite3VdbeMemShallowCopy(pOut, pIn1, MEM_Ephem);
#ifdef SQLITE_DEBUG
  if( pOut->pScopyFrom==0 ) pOut->pScopyFrom = pIn1;
#endif
  // break;
}

/* Opcode: Affinity P1 P2 * P4 *
** Synopsis: affinity(r[P1@P2])
**
** Apply affinities to a range of P2 registers starting with P1.
**
** P4 is a string that is P2 characters long. The nth character of the
** string indicates the column affinity that should be used for the nth
** memory cell in the range.
*/
void impl_OP_Affinity(Vdbe *p, sqlite3 *db, Op *pOp) {
// case OP_Affinity: {
  const char *zAffinity;   /* The affinity to be applied */
  char cAff;               /* A single character of affinity */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  u8 encoding = ENC(db);     /* The database encoding */

  zAffinity = pOp->p4.z;
  assert( zAffinity!=0 );
  assert( zAffinity[pOp->p2]==0 );
  pIn1 = &aMem[pOp->p1];
  while( (cAff = *(zAffinity++))!=0 ){
    assert( pIn1 <= &p->aMem[(p->nMem-p->nCursor)] );
    assert( memIsValid(pIn1) );
    applyAffinity(pIn1, cAff, encoding);
    pIn1++;
  }
  // break;
}

/* Opcode: MakeRecord P1 P2 P3 P4 *
** Synopsis: r[P3]=mkrec(r[P1@P2])
**
** Convert P2 registers beginning with P1 into the [record format]
** use as a data record in a database table or as a key
** in an index.  The OP_Column opcode can decode the record later.
**
** P4 may be a string that is P2 characters long.  The nth character of the
** string indicates the column affinity that should be used for the nth
** field of the index key.
**
** The mapping from character to affinity is given by the SQLITE_AFF_
** macros defined in sqliteInt.h.
**
** If P4 is NULL then all index fields have the affinity NONE.
*/
long impl_OP_MakeRecord(Vdbe *p, sqlite3 *db, long pc, long rc, Op *pOp) {
// case OP_MakeRecord: {
  u8 *zNewRecord;        /* A buffer to hold the data for the new record */
  Mem *pRec;             /* The new record */
  u64 nData;             /* Number of bytes of data space */
  int nHdr;              /* Number of bytes of header space */
  i64 nByte;             /* Data space required for this record */
  int nZero;             /* Number of zero bytes at the end of the record */
  int nVarint;           /* Number of bytes in a varint */
  u32 serial_type;       /* Type field */
  Mem *pData0;           /* First field to be combined into the record */
  Mem *pLast;            /* Last field of the record */
  int nField;            /* Number of fields in the record */
  char *zAffinity;       /* The affinity string for the record */
  int file_format;       /* File format to use for encoding */
  int i;                 /* Space used in zNewRecord[] header */
  int j;                 /* Space used in zNewRecord[] content */
  int len;               /* Length of a field */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  u8 encoding = ENC(db);     /* The database encoding */

  /* Assuming the record contains N fields, the record format looks
  ** like this:
  **
  ** ------------------------------------------------------------------------
  ** | hdr-size | type 0 | type 1 | ... | type N-1 | data0 | ... | data N-1 | 
  ** ------------------------------------------------------------------------
  **
  ** Data(0) is taken from register P1.  Data(1) comes from register P1+1
  ** and so forth.
  **
  ** Each type field is a varint representing the serial type of the 
  ** corresponding data element (see sqlite3VdbeSerialType()). The
  ** hdr-size field is also a varint which is the offset from the beginning
  ** of the record to data0.
  */
  nData = 0;         /* Number of bytes of data space */
  nHdr = 0;          /* Number of bytes of header space */
  nZero = 0;         /* Number of zero bytes at the end of the record */
  nField = pOp->p1;
  zAffinity = pOp->p4.z;
  assert( nField>0 && pOp->p2>0 && pOp->p2+nField<=(p->nMem-p->nCursor)+1 );
  pData0 = &aMem[nField];
  nField = pOp->p2;
  pLast = &pData0[nField-1];
  file_format = p->minWriteFileFormat;

  /* Identify the output register */
  assert( pOp->p3<pOp->p1 || pOp->p3>=pOp->p1+pOp->p2 );
  pOut = &aMem[pOp->p3];
  memAboutToChange(p, pOut);

  /* Apply the requested affinity to all inputs
  */
  assert( pData0<=pLast );
  if( zAffinity ){
    pRec = pData0;
    do{
      applyAffinity(pRec++, *(zAffinity++), encoding);
      assert( zAffinity[0]==0 || pRec<=pLast );
    }while( zAffinity[0] );
  }

  /* Loop through the elements that will make up the record to figure
  ** out how much space is required for the new record.
  */
  pRec = pLast;
  do{
    assert( memIsValid(pRec) );
    pRec->uTemp = serial_type = sqlite3VdbeSerialType(pRec, file_format);
    len = sqlite3VdbeSerialTypeLen(serial_type);
    if( pRec->flags & MEM_Zero ){
      if( nData ){
        sqlite3VdbeMemExpandBlob(pRec);
      }else{
        nZero += pRec->u.nZero;
        len -= pRec->u.nZero;
      }
    }
    nData += len;
    testcase( serial_type==127 );
    testcase( serial_type==128 );
    nHdr += serial_type<=127 ? 1 : sqlite3VarintLen(serial_type);
  }while( (--pRec)>=pData0 );

  /* EVIDENCE-OF: R-22564-11647 The header begins with a single varint
  ** which determines the total number of bytes in the header. The varint
  ** value is the size of the header in bytes including the size varint
  ** itself. */
  testcase( nHdr==126 );
  testcase( nHdr==127 );
  if( nHdr<=126 ){
    /* The common case */
    nHdr += 1;
  }else{
    /* Rare case of a really large header */
    nVarint = sqlite3VarintLen(nHdr);
    nHdr += nVarint;
    if( nVarint<sqlite3VarintLen(nHdr) ) nHdr++;
  }
  nByte = nHdr+nData;
  if( nByte>db->aLimit[SQLITE_LIMIT_LENGTH] ){
    // goto too_big;
    printf("In impl_OP_MakeRecord(): too_big.\n");
    rc = (long)gotoTooBig(p, db, (int)pc);
    return rc;    

  }

  /* Make sure the output register has a buffer large enough to store 
  ** the new record. The output register (pOp->p3) is not allowed to
  ** be one of the input registers (because the following call to
  ** sqlite3VdbeMemClearAndResize() could clobber the value before it is used).
  */
  if( sqlite3VdbeMemClearAndResize(pOut, (int)nByte) ){
    // goto no_mem;
    printf("In impl_OP_MakeRecord(): no_mem.\n");
    rc = (long)gotoNoMem(p, db, (int)pc);
    return rc;                        
  }
  zNewRecord = (u8 *)pOut->z;

  /* Write the record */
  i = putVarint32(zNewRecord, nHdr);
  j = nHdr;
  assert( pData0<=pLast );
  pRec = pData0;
  do{
    serial_type = pRec->uTemp;
    /* EVIDENCE-OF: R-06529-47362 Following the size varint are one or more
    ** additional varints, one per column. */    
    i += putVarint32(&zNewRecord[i], serial_type);            /* serial type */
    /* EVIDENCE-OF: R-64536-51728 The values for each column in the record
    ** immediately follow the header. */
    j += sqlite3VdbeSerialPut(&zNewRecord[j], pRec, serial_type); /* content */
  }while( (++pRec)<=pLast );
  assert( i==nHdr );
  assert( j==nByte );

  assert( pOp->p3>0 && pOp->p3<=(p->nMem-p->nCursor) );
  pOut->n = (int)nByte;
  pOut->flags = MEM_Blob;
  if( nZero ){
    pOut->u.nZero = nZero;
    pOut->flags |= MEM_Zero;
  }
  pOut->enc = SQLITE_UTF8;  /* In case the blob is ever converted to text */
  REGISTER_TRACE(pOp->p3, pOut);
  UPDATE_MAX_BLOBSIZE(pOut);
  // break;
  return rc;
}


/* Opcode:  Gosub P1 P2 * * *
**
** Write the current address onto register P1
** and then jump to address P2.
*/
long impl_OP_Gosub(Vdbe *p, long pc, Op *pOp) {
// case OP_Gosub: {            /* jump */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  assert( pOp->p1>0 && pOp->p1<=(p->nMem-p->nCursor) );
  pIn1 = &aMem[pOp->p1];
  assert( VdbeMemDynamic(pIn1)==0 );
  memAboutToChange(p, pIn1);
  pIn1->flags = MEM_Int;
  pIn1->u.i = pc;
  REGISTER_TRACE(pOp->p1, pIn1);
  pc = (long)pOp->p2 - 1;
  // break;
  return pc;
}

/* Opcode:  Return P1 * * * *
**
** Jump to the next instruction after the address in register P1.  After
** the jump, register P1 becomes undefined.
*/
long impl_OP_Return(Vdbe *p, long pc, Op *pOp) {
// case OP_Return: {           /* in1 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags==MEM_Int );
  pc = (long)pIn1->u.i;
  pIn1->flags = MEM_Undefined;
  // break;
  return pc;
}

long impl_OP_NextIfOpen(Vdbe *p, sqlite3 *db, long *pc, long rc, Op *pOp) {
// case OP_NextIfOpen:    /* jump */
  if( p->apCsr[pOp->p1]==0 ) {
    // break;
    return rc;
  }
  /* Fall through */
// case OP_Prev:           jump 
// case OP_Next:          /* jump */
  return impl_OP_Next(p, db, pc, pOp);
}

/* Opcode: Sequence P1 P2 * * *
** Synopsis: r[P2]=cursor[P1].ctr++
**
** Find the next available sequence number for cursor P1.
** Write the sequence number into register P2.
** The sequence number on the cursor is incremented after this
** instruction.  
*/
void impl_OP_Sequence(Vdbe *p, Op *pOp) {
// case OP_Sequence: {           /* out2-prerelease */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */
  pOut = &aMem[pOp->p2];
  pOut->flags = MEM_Int;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( p->apCsr[pOp->p1]!=0 );
  pOut->u.i = p->apCsr[pOp->p1]->seqCount++;
  // break;
}

/* Opcode: Compare P1 P2 P3 P4 P5
** Synopsis: r[P1@P3] <-> r[P2@P3]
**
** Compare two vectors of registers in reg(P1)..reg(P1+P3-1) (call this
** vector "A") and in reg(P2)..reg(P2+P3-1) ("B").  Save the result of
** the comparison for use by the next OP_Jump instruct.
**
** If P5 has the OPFLAG_PERMUTE bit set, then the order of comparison is
** determined by the most recent OP_Permutation operator.  If the
** OPFLAG_PERMUTE bit is clear, then register are compared in sequential
** order.
**
** P4 is a KeyInfo structure that defines collating sequences and sort
** orders for the comparison.  The permutation applies to registers
** only.  The KeyInfo elements are used sequentially.
**
** The comparison is a sort comparison, so NULLs compare equal,
** NULLs are less than numbers, numbers are less than strings,
** and strings are less than blobs.
*/
void impl_OP_Compare(Vdbe *p, Op *pOp) {
// case OP_Compare: {
  int n;
  int i;
  int p1;
  int p2;
  const KeyInfo *pKeyInfo;
  int idx;
  CollSeq *pColl;    /* Collating sequence to use on this term */
  int bRev;          /* True for DESCENDING sort order */

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  int *aPermute = 0;         /* Permutation of columns for OP_Compare */

  // From OP_Permutation
  assert( pOp->p4.ai );
  aPermute = pOp->p4.ai;

  if( (pOp->p5 & OPFLAG_PERMUTE)==0 ) aPermute = 0;
  n = pOp->p3;
  pKeyInfo = pOp->p4.pKeyInfo;
  assert( n>0 );
  assert( pKeyInfo!=0 );
  p1 = pOp->p1;
  p2 = pOp->p2;
#if SQLITE_DEBUG
  if( aPermute ){
    int k, mx = 0;
    for(k=0; k<n; k++) if( aPermute[k]>mx ) mx = aPermute[k];
    assert( p1>0 && p1+mx<=(p->nMem-p->nCursor)+1 );
    assert( p2>0 && p2+mx<=(p->nMem-p->nCursor)+1 );
  }else{
    assert( p1>0 && p1+n<=(p->nMem-p->nCursor)+1 );
    assert( p2>0 && p2+n<=(p->nMem-p->nCursor)+1 );
  }
#endif /* SQLITE_DEBUG */
  for(i=0; i<n; i++){
    idx = aPermute ? aPermute[i] : i;
    assert( memIsValid(&aMem[p1+idx]) );
    assert( memIsValid(&aMem[p2+idx]) );
    REGISTER_TRACE(p1+idx, &aMem[p1+idx]);
    REGISTER_TRACE(p2+idx, &aMem[p2+idx]);
    assert( i<pKeyInfo->nField );
    pColl = pKeyInfo->aColl[i];
    bRev = pKeyInfo->aSortOrder[i];
    iCompare = sqlite3MemCompare(&aMem[p1+idx], &aMem[p2+idx], pColl);
    if( iCompare ){
      if( bRev ) iCompare = -iCompare;
      // break;
      return;
    }
  }
  aPermute = 0;
  // break;
}

/* Opcode: Jump P1 P2 P3 * *
**
** Jump to the instruction at address P1, P2, or P3 depending on whether
** in the most recent OP_Compare instruction the P1 vector was less than
** equal to, or greater than the P2 vector, respectively.
*/
long impl_OP_Jump(Op *pOp) {
// case OP_Jump: {             /* jump */
  int pc;

  if( iCompare<0 ){
    pc = pOp->p1 - 1;  VdbeBranchTaken(0,3);
  }else if( iCompare==0 ){
    pc = pOp->p2 - 1;  VdbeBranchTaken(1,3);
  }else{
    pc = pOp->p3 - 1;  VdbeBranchTaken(2,3);
  }
  // break;
  return (long)pc;
}

/* Opcode: IfPos P1 P2 * * *
** Synopsis: if r[P1]>0 goto P2
**
** If the value of register P1 is 1 or greater, jump to P2.
**
** It is illegal to use this instruction on a register that does
** not contain an integer.  An assertion fault will result if you try.
*/
long impl_OP_IfPos(Vdbe *p, long pc, Op *pOp) {
// case OP_IfPos: {        /* jump, in1 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags&MEM_Int );
  VdbeBranchTaken( pIn1->u.i>0, 2);
  if( pIn1->u.i>0 ){
     pc = (long)pOp->p2 - 1;
  }
  // break;
  return pc;
}

/* Opcode: CollSeq P1 * * P4
**
** P4 is a pointer to a CollSeq struct. If the next call to a user function
** or aggregate calls sqlite3GetFuncCollSeq(), this collation sequence will
** be returned. This is used by the built-in min(), max() and nullif()
** functions.
**
** If P1 is not zero, then it is a register that a subsequent min() or
** max() aggregate will set to 1 if the current row is not the minimum or
** maximum.  The P1 register is initialized to 0 by this instruction.
**
** The interface used by the implementation of the aforementioned functions
** to retrieve the collation sequence set by this opcode is not available
** publicly, only to user functions defined in func.c.
*/
void impl_OP_CollSeq(Vdbe *p, Op *pOp) {
// case OP_CollSeq: {
  Mem *aMem = p->aMem;       /* Copy of p->aMem */

  assert( pOp->p4type==P4_COLLSEQ );
  if( pOp->p1 ){
    sqlite3VdbeMemSetInt64(&aMem[pOp->p1], 0);
  }
  // break;
}

/* Opcode: NotNull P1 P2 * * *
** Synopsis: if r[P1]!=NULL goto P2
**
** Jump to P2 if the value in register P1 is not NULL.  
*/
long impl_OP_NotNull(Vdbe *p, long pc, Op *pOp) {
// case OP_NotNull: {            /* same as TK_NOTNULL, jump, in1 */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  VdbeBranchTaken( (pIn1->flags & MEM_Null)==0, 2);
  if( (pIn1->flags & MEM_Null)==0 ){
    pc = (long)pOp->p2 - 1;
  }
  // break;
  return pc;
}

/* Opcode: InitCoroutine P1 P2 P3 * *
**
** Set up register P1 so that it will OP_Yield to the co-routine
** located at address P3.
**
** If P2!=0 then the co-routine implementation immediately follows
** this opcode.  So jump over the co-routine implementation to
** address P2.
*/
long impl_OP_InitCoroutine(Vdbe *p, long pc, Op *pOp) {
// case OP_InitCoroutine: {     /* jump */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pOut;                 /* Output operand */

  assert( pOp->p1>0 &&  pOp->p1<=(p->nMem-p->nCursor) );
  assert( pOp->p2>=0 && pOp->p2<p->nOp );
  assert( pOp->p3>=0 && pOp->p3<p->nOp );
  pOut = &aMem[pOp->p1];
  assert( !VdbeMemDynamic(pOut) );
  pOut->u.i = pOp->p3 - 1;
  pOut->flags = MEM_Int;
  if( pOp->p2 ) pc = pOp->p2 - 1;
  // break;
  return pc;
}

/* Opcode:  Yield P1 P2 * * *
**
** Swap the program counter with the value in register P1.
**
** If the co-routine ends with OP_Yield or OP_Return then continue
** to the next instruction.  But if the co-routine ends with
** OP_EndCoroutine, jump immediately to P2.
*/
long impl_OP_Yield(Vdbe *p, long pc, Op *pOp) {
// case OP_Yield: {            /* in1, jump */
  int pcDest;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */

  pIn1 = &aMem[pOp->p1];
  assert( VdbeMemDynamic(pIn1)==0 );
  pIn1->flags = MEM_Int;
  pcDest = (int)pIn1->u.i;
  pIn1->u.i = pc;
  REGISTER_TRACE(pOp->p1, pIn1);
  pc = (long)pcDest;
  // break;
  return pc;
}

/* Opcode:  EndCoroutine P1 * * * *
**
** The instruction at the address in register P1 is an OP_Yield.
** Jump to the P2 parameter of that OP_Yield.
** After the jump, register P1 becomes undefined.
*/
long impl_OP_EndCoroutine(Vdbe *p, Op *pOp) {
// case OP_EndCoroutine: {           /* in1 */
  VdbeOp *pCaller;

  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1;                 /* 1st input operand */
  Op *aOp = p->aOp;          /* Copy of p->aOp */
  long pc;

  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags==MEM_Int );
  assert( pIn1->u.i>=0 && pIn1->u.i<p->nOp );
  pCaller = &aOp[pIn1->u.i];
  assert( pCaller->opcode==OP_Yield );
  assert( pCaller->p2>=0 && pCaller->p2<p->nOp );
  pc = pCaller->p2 - 1;
  pIn1->flags = MEM_Undefined;
  // break;
  return pc;
}

/* Opcode: Variable P1 P2 * P4 *
** Synopsis: r[P2]=parameter(P1,P4)
**
** Transfer the values of bound parameter P1 into register P2
**
** If the parameter is named, then its name appears in P4.
** The P4 value is used by sqlite3_bind_parameter_name().
*/
long impl_OP_Variable(Vdbe* p, sqlite3 *db, long pc, long rc, Op *pOp) {
  Mem *pVar;       /* Value being transferred */
  Mem *pOut;                 /* Output operand */
  Mem *aMem = p->aMem;
  pOut = &aMem[pOp->p2];

  assert( pOp->p1>0 && pOp->p1<=p->nVar );
  assert( pOp->p4.z==0 || pOp->p4.z==p->azVar[pOp->p1-1] );
  pVar = &p->aVar[pOp->p1 - 1];
  if( sqlite3VdbeMemTooBig(pVar) ){
    printf("In impl_OP_Variable(): too_big.\n");
    rc = gotoTooBig(p, db, (int)pc);
    return (long)rc;
  }
  sqlite3VdbeMemShallowCopy(pOut, pVar, MEM_Static);
  UPDATE_MAX_BLOBSIZE(pOut);
  return (long)rc;
}


/* Opcode: Cast P1 P2 * * *
** Synopsis: affinity(r[P1])
**
** Force the value in register P1 to be the type defined by P2.
**
** <ul>
** <li value="97"> TEXT
** <li value="98"> BLOB
** <li value="99"> NUMERIC
** <li value="100"> INTEGER
** <li value="101"> REAL
** </ul>
**
** A NULL value is not changed by this routine.  It remains NULL.
*/
long impl_OP_Cast(Vdbe* p, sqlite3 *db, long rc, Op *pOp) {
  Mem *pIn1;
  Mem *aMem = p->aMem;
  u8 encoding = ENC(db);     /* The database encoding */
  pIn1 = &aMem[pOp->p1];
  memAboutToChange(p, pIn1);
  rc = ExpandBlob(pIn1);
  sqlite3VdbeMemCast(pIn1, pOp->p2, encoding);
  UPDATE_MAX_BLOBSIZE(pIn1);
  return (long)rc;
}
