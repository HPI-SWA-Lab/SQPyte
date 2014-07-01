from rpython.rtyper.lltypesystem import rffi
from sqpyte.interpreter import Sqlite3DB, Sqlite3Query
from sqpyte.capi import CConfig
from sqpyte import capi
from sqpyte.translated import allocateCursor, sqlite3VdbeMemIntegerify, sqlite3BtreeCursor
from sqpyte.translated import sqlite3BtreeCursorHints, sqlite3VdbeSorterRewind
import os, sys

testdb = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test.db")
tpchdb = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tpch.db")

def test_opendb():
    db = Sqlite3DB(testdb).db
    assert db

def test_prepare():
    db = Sqlite3DB(testdb).db
    query = Sqlite3Query(db, 'select * from contacts;')
    assert query.p and query.db
    assert query.p.db == query.db
    assert query.p.nOp == 17

    assert query.p.aOp[0].opcode == 155
    assert query.p.aOp[0].p1 == 0
    assert query.p.aOp[0].p2 == 14
    assert query.p.aOp[0].p3 == 0

    assert query.p.aOp[1].opcode == 52
    assert query.p.aOp[1].p1 == 0
    assert query.p.aOp[1].p2 == 2
    assert query.p.aOp[1].p3 == 0

    assert query.p.aOp[2].opcode == 105
    assert query.p.aOp[2].p1 == 0
    assert query.p.aOp[2].p2 == 12
    assert query.p.aOp[2].p3 == 0

def test_multiple_queries():
    db = Sqlite3DB(testdb).db
    query = Sqlite3Query(db, 'select name from contacts;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 100)
    query = Sqlite3Query(db, 'select name from contacts where age > 50;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 48)

def test_reset():
    db = Sqlite3DB(testdb).db
    query = Sqlite3Query(db, 'select name from contacts;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.python_sqlite3_column_bytes(0)
    name = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.python_sqlite3_column_text(0)), textlen)
    query.reset_query()
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.python_sqlite3_column_bytes(0)
    name2 = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.python_sqlite3_column_text(0)), textlen)
    assert name == name2
    
def test_mainloop_over50():
    db = Sqlite3DB(testdb).db
    query = Sqlite3Query(db, 'select name from contacts where age > 50;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 48)

def test_mainloop_namelist():
    fname = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'names.txt')
    names = [name.strip() for name in open(fname)]
    db = Sqlite3DB(testdb).db
    query = Sqlite3Query(db, 'select name from contacts;')
    rc = query.mainloop()
    i = 0
    while rc == CConfig.SQLITE_ROW:
        textlen = query.python_sqlite3_column_bytes(0)
        name = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.python_sqlite3_column_text(0)), textlen)
        rc = query.mainloop()
        assert(name == names[i])
        i += 1
    assert(len(names) == i)

def test_count():
    db = Sqlite3DB(testdb).db
    query = Sqlite3Query(db, 'select count(name) from contacts where age > 20;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.python_sqlite3_column_bytes(0)
    count = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.python_sqlite3_column_text(0)), textlen)
    assert int(count) == 76


def test_join():
    db = Sqlite3DB(tpchdb).db
    query = Sqlite3Query(db, 'select S.name, N.name from Supplier S, Nation N where S.nationkey = N.nationkey;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    i = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        i += 1
    assert i == 100

def test_query_6():
    db = Sqlite3DB(tpchdb).db
    queryStr = ("select "
                    "sum(l.extendedprice * l.discount) as revenue "
                "from "
                    "lineitem l "
                "where "
                    "l.shipdate >= date('1996-01-01') "
                    "and l.shipdate < date('1996-01-01', '+1 year') "
                    "and l.discount between 0.04 and 0.07 "
                    "and l.quantity < 25;"
        )
    query = Sqlite3Query(db, queryStr)
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.python_sqlite3_column_bytes(0)
    result = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.python_sqlite3_column_text(0)), textlen)
    assert float(result) == 1524721.6695

def test_query_14():
    db = Sqlite3DB(tpchdb).db
    queryStr = ("select "
                    "100.00 * sum(case "
                        "when p.type like 'PROMO%' "
                            "then l.extendedprice * (1 - l.discount) "
                        "else 0 "
                    "end) / sum(l.extendedprice * (1 - l.discount)) as promo_revenue "
                "from "
                    "lineitem l, "
                    "part p "
                "where "
                    "l.partkey = p.partkey "
                    "and l.shipdate >= date('1995-01-01') "
                    "and l.shipdate < date('1995-01-01', '+1 month');"
        )
    query = Sqlite3Query(db, queryStr)
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.python_sqlite3_column_bytes(0)
    result = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.python_sqlite3_column_text(0)), textlen)
    assert float(result) == 15.9871053076363

def test_translated_allocateCursor():
    db = Sqlite3DB(testdb).db
    p = Sqlite3Query(db, 'select name from contacts;').p
    vdbe = allocateCursor(p, p.aOp[0].p1, p.aOp[0].p4.i, p.aOp[0].p3, 1)

def test_translated_sqlite3VdbeMemIntegerify():
    db = Sqlite3DB(testdb).db
    p = Sqlite3Query(db, 'select name from contacts;').p
    pOp = p.aOp[0]
    p2 = pOp.p2
    aMem = p.aMem
    pMem = aMem[p2]
    rc = sqlite3VdbeMemIntegerify(pMem)
    assert(rc == CConfig.SQLITE_OK)

def test_translated_sqlite3BtreeCursor():
    db = Sqlite3DB(testdb).db
    p = Sqlite3Query(db, 'select name from contacts;').p
    pOp = p.aOp[0]
    p2 = pOp.p2
    iDb = pOp.p3
    pDb = db.aDb[iDb]
    pX = pDb.pBt
    wrFlag = 1
    pKeyInfo = pOp.p4.pKeyInfo
    nField = p.aOp[0].p4.i
    pCur = allocateCursor(p, pOp.p1, nField, iDb, 1)
    pCur.nullRow = rffi.r_uchar(1)
    pCur.isOrdered = bool(1)
    rc = sqlite3BtreeCursor(pX, p2, wrFlag, pKeyInfo, pCur.pCursor)
    assert(rc == CConfig.SQLITE_OK)

def test_translated_sqlite3BtreeCursorHints():
    db = Sqlite3DB(testdb).db
    p = Sqlite3Query(db, 'select name from contacts;').p
    pOp = p.aOp[0]
    iDb = pOp.p3
    nField = p.aOp[0].p4.i
    pCur = allocateCursor(p, pOp.p1, nField, iDb, 1)
    sqlite3BtreeCursorHints(pCur.pCursor, (pOp.p5 & CConfig.OPFLAG_BULKCSR))

#
# NOTE: Currently sqlite3VdbeSorterRewind() function is not used and segfaults.
#
# def test_translated_sqlite3VdbeSorterRewind():
#     db = Sqlite3DB(testdb).db
#     p = Sqlite3Query(db, 'select name from contacts;').p
#     pOp = p.aOp[0]
#     p2 = pOp.p2
#     iDb = pOp.p3
#     pDb = db.aDb[iDb]
#     pX = pDb.pBt
#     wrFlag = 1
#     pKeyInfo = pOp.p4.pKeyInfo
#     nField = p.aOp[0].p4.i
#     pCur = allocateCursor(p, pOp.p1, nField, iDb, 1)
#     pCur.nullRow = rffi.r_uchar(1)
#     pCur.isOrdered = bool(1)
#     rc = sqlite3BtreeCursor(pX, p2, wrFlag, pKeyInfo, pCur.pCursor)
#     sqlite3BtreeCursorHints(pCur.pCursor, (pOp.p5 & CConfig.OPFLAG_BULKCSR))
#     pC = p.apCsr[pOp.p1]
#     res = 1
#     rc = sqlite3VdbeSorterRewind(db, pC, res)
#     assert(rc == CConfig.SQLITE_OK)

