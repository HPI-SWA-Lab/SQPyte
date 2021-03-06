import pytest
from rpython.rtyper.lltypesystem import rffi
from sqpyte.interpreter import SQPyteDB, SQPyteQuery
from sqpyte.capi import CConfig
from sqpyte import capi
from sqpyte.translated import allocateCursor, sqlite3BtreeCursor
from sqpyte.translated import sqlite3BtreeCursorHints, sqlite3VdbeSorterRewind
import os, sys

testdb = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test.db")
# testdb = os.path.join(os.path.dirname(os.path.abspath(__file__)), "big-test.db")

def test_opendb():
    db = SQPyteDB(testdb).db
    assert db

def test_prepare():
    db = SQPyteDB(testdb)
    query = db.execute('select * from contacts;')
    assert query.p and query.db
    assert query.p.db == query.db
    assert query.p.nOp == 17

    assert query.p.aOp[0].opcode == CConfig.OP_Init
    assert query.p.aOp[0].p1 == 0
    assert query.p.aOp[0].p2 == 14
    assert query.p.aOp[0].p3 == 0

    assert query.p.aOp[1].opcode == CConfig.OP_OpenRead
    assert query.p.aOp[1].p1 == 0
    assert query.p.aOp[1].p2 == 2
    assert query.p.aOp[1].p3 == 0

    assert query.p.aOp[2].opcode == CConfig.OP_Rewind
    assert query.p.aOp[2].p1 == 0
    assert query.p.aOp[2].p2 == 12
    assert query.p.aOp[2].p3 == 0

def test_multiple_queries():
    db = SQPyteDB(testdb)
    query = db.execute('select name from contacts;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 100)
    query = db.execute('select name from contacts where age > 50;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 48)

def test_reset():
    db = SQPyteDB(testdb)
    query = db.execute('select name from contacts;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    name = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    query.reset_query()
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    name2 = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    assert name == name2

def test_mainloop_over50():
    db = SQPyteDB(testdb)
    query = db.execute('select name from contacts where age > 50;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 48)

def test_mainloop_arithmetic():
    db = SQPyteDB(testdb)
    query = db.execute('select name from contacts where 2 * age + 2 - age / 1 > 48;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 53)

def test_mainloop_mixed_arithmetic():
    db = SQPyteDB(testdb)
    query = db.execute('select name from contacts where 2.1 * age + 2 - age / 0.909 > 48;')
    rc = query.mainloop()
    count = 0
    while rc == CConfig.SQLITE_ROW:
        rc = query.mainloop()
        count += 1
    assert(count == 53)

def test_count_avg_sum():
    db = SQPyteDB(testdb)
    query = db.execute('select count(*), avg(age), sum(age) from contacts where 2 * age + 2 - age / 1 > 48;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    count = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    assert count == "53"
    textlen = query.column_bytes(1)
    avg = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(1)), textlen)
    assert avg == "72.5283018867924"
    textlen = query.column_bytes(2)
    sum = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(2)), textlen)
    assert sum == "3844"

def test_select_without_from():
    db = SQPyteDB(testdb)
    query = db.execute('select 1, 2;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW

def test_mainloop_namelist():
    fname = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'names.txt')
    names = [name.strip() for name in open(fname)]
    db = SQPyteDB(testdb)
    query = db.execute('select name from contacts;')
    rc = query.mainloop()
    i = 0
    while rc == CConfig.SQLITE_ROW:
        textlen = query.column_bytes(0)
        name = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
        rc = query.mainloop()
        assert(name == names[i])
        i += 1
    assert(len(names) == i)

def test_count():
    db = SQPyteDB(testdb)
    query = db.execute('select count(name) from contacts where age > 20;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    count = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    assert int(count) == 76

def test_null_comparison():
    db = SQPyteDB(testdb)
    query = db.execute('select count(*) from contacts where age > 10 and age < 14;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    count = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    assert int(count) == 3

def test_comparison():
    db = SQPyteDB(testdb)
    query = db.execute('select count(*) from contacts where age > 40 and age < 60;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    count = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    assert int(count) == 18

def test_string_comparison():
    db = SQPyteDB(testdb)
    query = db.execute("select count(*) from contacts where name = 'Raphael Paul';")
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    count = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    assert int(count) == 1    

def test_makerecord():
    db = SQPyteDB(testdb)
    query = db.execute("select age, name from contacts order by age, age * 0.5;")
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(1)
    name = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(1)), textlen)
    assert name == "Jermaine Mayo"

def test_translated_allocateCursor():
    db = SQPyteDB(testdb)
    p = db.execute('select name from contacts;').p
    vdbe = allocateCursor(p, p.aOp[0].p1, p.aOp[0].p4.i, p.aOp[0].p3, 1)

def test_translated_sqlite3BtreeCursorHints():
    db = SQPyteDB(testdb)
    p = db.execute('select name from contacts;').p
    pOp = p.aOp[0]
    iDb = pOp.p3
    nField = p.aOp[0].p4.i
    pCur = allocateCursor(p, pOp.p1, nField, iDb, 1)
    sqlite3BtreeCursorHints(pCur.pCursor, (pOp.p5 & CConfig.OPFLAG_BULKCSR))

#
# NOTE: Currently sqlite3VdbeSorterRewind() function is not used and segfaults.
#
# def test_translated_sqlite3VdbeSorterRewind():
#     db = SQPyteDB(testdb)
#     p = db.execute('select name from contacts;').p
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

def test_real():
    db = SQPyteDB(':memory:')
    query = db.execute('select 2.3 + 4.5;')
    rc = query.mainloop()
    res = query.column_double(0)
    assert res == 2.3 + 4.5

def test_mandelbrot():
    pytest.skip()
    s = """
    WITH RECURSIVE
      xaxis(x) AS (VALUES(-2.0) UNION ALL SELECT x+0.5 FROM xaxis WHERE x<1.2),
      yaxis(y) AS (VALUES(-1.0) UNION ALL SELECT y+0.6 FROM yaxis WHERE y<1.0),
      m(iter, cx, cy, x, y) AS (
        SELECT 0, x, y, 0.0, 0.0 FROM xaxis, yaxis
        UNION ALL
        SELECT iter+1, cx, cy, x*x-y*y + cx, 2.0*x*y + cy FROM m
         WHERE (x*x + y*y) < 4.0 AND iter<28
      ),
      m2(iter, cx, cy) AS (
        SELECT max(iter), cx, cy FROM m GROUP BY cx, cy
      ),
      a(t) AS (
        SELECT group_concat( substr(' .+*#', 1+min(iter/7,4), 1), '')
        FROM m2 GROUP BY cy
      )
    SELECT group_concat(rtrim(t),x'0a') FROM a;
    """
    db = SQPyteDB(":memory:")
    query = db.execute(s)
    rc = query.mainloop()


def test_nqueens():
    pytest.skip()
    s = """
    WITH RECURSIVE
      positions(i) as (
        VALUES(0)
        UNION SELECT ALL
        i+1 FROM positions WHERE i < 63
        ),
      solutions(board, n_queens) AS (
        SELECT '----------------------------------------------------------------', cast(0 AS bigint)
          FROM positions
        UNION
        SELECT
          substr(board, 1, i) || '*' || substr(board, i+2),n_queens + 1 as n_queens
          FROM positions AS ps, solutions
        WHERE n_queens < 8
          AND substr(board,1,i) != '*'
          AND NOT EXISTS (
            SELECT 1 FROM positions WHERE
              substr(board,i+1,1) = '*' AND
                (
                    i % 8 = ps.i %8 OR
                    cast(i / 8 AS INT) = cast(ps.i / 8 AS INT) OR
                    cast(i / 8 AS INT) + (i % 8) = cast(ps.i / 8 AS INT) + (ps.i % 8) OR
                    cast(i / 8 AS INT) - (i % 8) = cast(ps.i / 8 AS INT) - (ps.i % 8)
                )
            LIMIT 1
            )
       ORDER BY n_queens DESC --remove this for PostgreSQL
      )

    -- Perform a selector over the CTE to extract the solutions with 8 queens
    SELECT board,n_queens FROM solutions WHERE n_queens = 8;
    """
    db = SQPyteDB(':memory:')
    query = db.execute(s)
    rc = query.mainloop()

def test_argument():
    def c():
        count = 0
        rc = query.mainloop()
        while rc == CConfig.SQLITE_ROW:
            rc = query.mainloop()
            count += 1
        return count
    db = SQPyteDB(testdb)
    query = db.execute('select name from contacts where age > ?;')
    query.bind_int64(1, 50)
    count = c()
    assert count == 48

    query.reset_query()
    query.bind_double(1, 75.5)
    count = c()
    assert count == 22

    query = db.execute('select age from contacts where name = ?;')
    query.bind_str(1, 'Dean Shepherd')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    assert query.column_int64(0) == 31

def test_create_table_and_insert():
    db = SQPyteDB(":memory:")
    query = db.execute('create table cos (x real, y real);')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_DONE
    query = db.execute('insert into cos values (0, 1)')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_DONE
    query = db.execute('delete from cos;')
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_DONE

def test_disable_cache():
    db = SQPyteDB(testdb)
    query = db.execute('select count(name) from contacts where age > 20;', use_flag_cache=False)
    rc = query.mainloop()
    assert rc == CConfig.SQLITE_ROW
    textlen = query.column_bytes(0)
    count = rffi.charpsize2str(rffi.cast(rffi.CCHARP, query.column_text(0)), textlen)
    assert int(count) == 76

