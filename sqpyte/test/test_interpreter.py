from sqpyte.interpreter import opendb, prepare

import os

testdb = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test.db")

def test_opendb():
	db = opendb(testdb)
	assert db

def test_prepare():
	db = opendb(testdb)
	stmt = prepare(db, 'select * from contacts;')
	assert stmt and db
	assert stmt.db == db
	assert stmt.nOp == 17

	assert stmt.aOp[0].opcode == 155
	assert stmt.aOp[0].p1 == 0
	assert stmt.aOp[0].p2 == 14
	assert stmt.aOp[0].p3 == 0


	assert stmt.aOp[1].opcode == 52
	assert stmt.aOp[1].p1 == 0
	assert stmt.aOp[1].p2 == 2
	assert stmt.aOp[1].p3 == 0

	assert stmt.aOp[2].opcode == 105
	assert stmt.aOp[2].p1 == 0
	assert stmt.aOp[2].p2 == 12
	assert stmt.aOp[2].p3 == 0
