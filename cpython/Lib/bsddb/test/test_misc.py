"""Miscellaneous bsddb module test cases
"""

import os
import shutil
import sys
import unittest
import tempfile

try:
    # For Pythons w/distutils pybsddb
    from bsddb3 import db, dbshelve, hashopen
except ImportError:
    # For the bundled bsddb
    from bsddb import db, dbshelve, hashopen

try:
    from bsddb3 import test_support
except ImportError:
    from test import test_support

#----------------------------------------------------------------------

class MiscTestCase(unittest.TestCase):
    def setUp(self):
        self.filename = self.__class__.__name__ + '.db'
        homeDir = os.path.join(tempfile.gettempdir(), 'db_home%d'%os.getpid())
        self.homeDir = homeDir
        try:
            os.mkdir(homeDir)
        except OSError:
            pass

    def tearDown(self):
        test_support.unlink(self.filename)
        test_support.rmtree(self.homeDir)

    def test01_badpointer(self):
        dbs = dbshelve.open(self.filename)
        dbs.close()
        self.assertRaises(db.DBError, dbs.get, b"foo")

    def test02_db_home(self):
        env = db.DBEnv()
        # check for crash fixed when db_home is used before open()
        assert env.db_home is None
        env.open(self.homeDir, db.DB_CREATE)
        assert self.homeDir == env.db_home

    def test03_repr_closed_db(self):
        db = hashopen(self.filename)
        db.close()
        rp = repr(db)
        self.assertEquals(rp, "{}")

    # http://sourceforge.net/tracker/index.php?func=detail&aid=1708868&group_id=13900&atid=313900
    #
    # See the bug report for details.
    #
    # The problem was that make_key_dbt() was not allocating a copy of
    # string keys but FREE_DBT() was always being told to free it when the
    # database was opened with DB_THREAD.
    def test04_double_free_make_key_dbt(self):
        try:
            db1 = db.DB()
            db1.open(self.filename, None, db.DB_BTREE,
                     db.DB_CREATE | db.DB_THREAD)

            curs = db1.cursor()
            t = curs.get(b"/foo", db.DB_SET)
            # double free happened during exit from DBC_get
        finally:
            db1.close()
            os.unlink(self.filename)

    def test05_key_with_null_bytes(self):
        try:
            db1 = db.DB()
            db1.open(self.filename, None, db.DB_HASH, db.DB_CREATE)
            db1[b'a'] = b'eh?'
            db1[b'a\x00'] = b'eh zed.'
            db1[b'a\x00a'] = b'eh zed eh?'
            db1[b'aaa'] = b'eh eh eh!'
            keys = db1.keys()
            keys.sort()
            self.assertEqual([b'a', b'a\x00', b'a\x00a', b'aaa'], keys)
            self.assertEqual(db1[b'a'], b'eh?')
            self.assertEqual(db1[b'a\x00'], b'eh zed.')
            self.assertEqual(db1[b'a\x00a'], b'eh zed eh?')
            self.assertEqual(db1[b'aaa'], b'eh eh eh!')
        finally:
            db1.close()
            os.unlink(self.filename)

    def test_DB_set_flags_persists(self):
        if db.version() < (4,2):
            # The get_flags API required for this to work is only available
            # in BerkeleyDB >= 4.2
            return
        try:
            db1 = db.DB()
            db1.set_flags(db.DB_DUPSORT)
            db1.open(self.filename, db.DB_HASH, db.DB_CREATE)
            db1[b'a'] = b'eh'
            db1[b'a'] = b'A'
            self.assertEqual([(b'a', b'A')], db1.items())
            db1.put(b'a', b'Aa')
            self.assertEqual([(b'a', b'A'), (b'a', b'Aa')], db1.items())
            db1.close()
            db1 = db.DB()
            # no set_flags call, we're testing that it reads and obeys
            # the flags on open.
            db1.open(self.filename, db.DB_HASH)
            self.assertEqual([(b'a', b'A'), (b'a', b'Aa')], db1.items())
            # if it read the flags right this will replace all values
            # for key b'a' instead of adding a new one.  (as a dict should)
            db1[b'a'] = b'new A'
            self.assertEqual([(b'a', b'new A')], db1.items())
        finally:
            db1.close()
            os.unlink(self.filename)


#----------------------------------------------------------------------


def test_suite():
    return unittest.makeSuite(MiscTestCase)


if __name__ == '__main__':
    unittest.main(defaultTest='test_suite')
