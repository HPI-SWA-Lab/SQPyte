from rpython.rlib import jit, rarithmetic, objectmodel, longlong2float
from rpython.rtyper.lltypesystem import rffi, lltype
from sqpyte.capi import CConfig
from sqpyte import capi
import sys, math

class Mem(object):
    _immutable_fields_ = ['hlquery', 'pMem', '_cache_index']
    _attrs_ = ['hlquery', 'pMem', '_cache_index', '_cache', '_python_ctx']

    def __init__(self, hlquery, pMem, _cache_index=-1):
        self.hlquery = hlquery
        self.pMem = pMem
        self._cache_index = _cache_index
        self._python_ctx = None

    def _debug_print(self):
        flags = self.get_flags()
        if flags & CConfig.MEM_Undefined:
            print "undefined"
        elif flags & CConfig.MEM_Null:
            print "NULL"
        elif flags & (CConfig.MEM_Int|CConfig.MEM_Str) == (CConfig.MEM_Int|CConfig.MEM_Str):
            print "si:%s" % self.get_u_i()
        elif flags & CConfig.MEM_Int:
            print "i:%s" % self.get_u_i()
        elif flags & (CConfig.MEM_Real|CConfig.MEM_Str) == (CConfig.MEM_Real|CConfig.MEM_Str):
            print "sr:%s" % self.get_u_r()
        elif flags & CConfig.MEM_Real:
            print "r:%s" % self.get_u_r()
        elif flags & CConfig.MEM_RowSet:
            print "(rowset)"
        else:
            result = []
            if flags & CConfig.MEM_Dyn:
                result.append('z')
            elif flags & CConfig.MEM_Static:
                result.append('t')
            elif flags & CConfig.MEM_Ephem:
                result.append('e')
            else:
                result.append('s')
            result.append(str(self.get_n()))
            result.append('[')
            z = self.get_z()
            for i in range(0, self.get_n()):
                c = z[i]
                result.append(c)
            result.append(']')
            if self.get_enc_signed() & CConfig.SQLITE_UTF8:
                result.append('(8)')
            print ''.join(result)

    def invalidate_cache(self):
        if self._cache_index != -1:
            self.hlquery.mem_cache.invalidate(self._cache_index)

    def check_cache_consistency(self):
        if self._cache_index == -1:
            return
        state = self.hlquery.mem_cache.cache_state()
        if state.is_flag_known(self._cache_index):
            assert rffi.cast(lltype.Unsigned, self.pMem.flags) == self.get_flags()
        if state.is_u_r_known(self._cache_index):
            assert self.pMem.u.r == self.get_u_r()
        if state.is_u_i_known(self._cache_index):
            assert self.pMem.u.i == self.get_u_i()

    def get_flags(self, promote=False):
        flags = self.hlquery.mem_cache.get_flags(self)
        if promote:
            jit.promote(flags)
        return flags

    def set_flags(self, newflags):
        self.hlquery.mem_cache.set_flags(self, newflags)

    def assure_flags(self, newflags):
        if not jit.we_are_jitted():
            assert self.pMem.flags == newflags
        self.hlquery.mem_cache.assure_flags(self, newflags)


    def get_u_i(self):
        return self.hlquery.mem_cache.get_u_i(self)

    def set_u_i(self, val, constant=False):
        return self.hlquery.mem_cache.set_u_i(self, val, constant)

    def get_u_r(self):
        return self.hlquery.mem_cache.get_u_r(self)

    def set_u_r(self, val, constant=False):
        return self.hlquery.mem_cache.set_u_r(self, val, constant)

    def is_constant_u_i(self):
        return self.hlquery.mem_cache.is_constant_u_i(self)

    def is_constant_u_r(self):
        return self.hlquery.mem_cache.is_constant_u_r(self)


    def get_u_nZero(self):
        return rffi.cast(lltype.Signed, self.pMem.u.nZero)

    def set_u_nZero(self, val):
        rffi.setintfield(self.pMem.u, 'nZero', val)

    def get_u_pFrame(self):
        return self.pMem.u.pFrame

    def set_u_pFrame(self, val):
        self.pMem.u.pFrame = val

    def get_u_pDef(self):
        return self.pMem.u.pDef

    def set_u_pDef(self, val):
        self.pMem.u.pDef = val

    def get_u_pRowSet(self):
        return self.pMem.u.pRowSet

    def set_u_pRowSet(self, val):
        self.pMem.u.pRowSet = val


    def get_n(self):
        return rffi.cast(lltype.Signed, self.pMem.n)

    def set_n(self, val):
        rffi.setintfield(self.pMem, 'n', val)

    def get_enc(self):
        return self.pMem.enc

    def get_enc_signed(self):
        return rffi.getintfield(self.pMem, 'enc')

    def set_enc(self, val):
        self.pMem.enc = val

    def set_enc_utf8(self):
        self.set_enc(rffi.cast(CConfig.u8, CConfig.SQLITE_UTF8))

    def get_z(self):
        return self.pMem.z

    def set_z(self, val):
        self.pMem.z = val

    def set_z_null(self):
        self.set_z(lltype.nullptr(rffi.CCHARP.TO))


    def get_zMalloc(self):
        return self.pMem.zMalloc

    def set_zMalloc(self, val):
        self.pMem.zMalloc = val

    def set_zMalloc_null(self):
        self.set_zMalloc(lltype.nullptr(lltype.typeOf(self.pMem).TO.zMalloc.TO))

    def get_szMalloc(self):
        return rffi.cast(lltype.Signed, self.pMem.szMalloc)

    def set_szMalloc(self, val):
        rffi.setintfield(self.pMem, 'szMalloc', val)


    def get_db(self):
        return self.pMem.db

    def set_db(self, db):
        self.pMem.db = db


    def get_xDel(self):
        return self.pMem.xDel

    def set_xDel(self, val):
        self.pMem.xDel = val

    def set_xDel_null(self):
        self.set_xDel(lltype.nullptr(lltype.typeOf(self.pMem).TO.xDel.TO))

    # _______________________________________________________________
    # methods induced by sqlite3 functions below

    def _atoi(self):
        val2 = lltype.malloc(rffi.LONGLONGP.TO, 1, flavor='raw')
        val2[0] = 0
        rc = capi.sqlite3Atoi64(self.get_z(), val2, self.get_n(), self.get_enc())
        value = val2[0]
        lltype.free(val2, flavor='raw')
        return rc, value

    def sqlite3VdbeMemCast(self, aff, encoding):
        """
        Cast the datatype of the value in pMem according to the affinity
        "aff".  Casting is different from applying affinity in that a cast
        is forced.  In other words, the value is converted into the desired
        affinity even if that results in loss of data.  This routine is
        used (for example) to implement the SQL "cast()" operator.
        """
        flags = self.get_flags()
        if flags & CConfig.MEM_Null:
            return
        if aff == CConfig.SQLITE_AFF_NONE:
            assert 0, "implement me!"
            #if( (flags & CConfig.MEM_Blob)==0 ){
            #  sqlite3ValueApplyAffinity(pMem, SQLITE_AFF_TEXT, encoding);
            #  assert( flags & CConfig.MEM_Str || pMem->db->mallocFailed );
            #  MemSetTypeFlag(pMem, CConfig.MEM_Blob);
            #}else{
            #  flags &= ~(CConfig.MEM_TypeMask&~CConfig.MEM_Blob);
            #}
            #break;
        elif aff == CConfig.SQLITE_AFF_NUMERIC:
            self.sqlite3VdbeMemNumerify()
        elif aff == CConfig.SQLITE_AFF_INTEGER:
            self.sqlite3VdbeMemIntegerify()
        elif aff == CConfig.SQLITE_AFF_REAL:
            self.sqlite3VdbeMemRealify()
        elif aff == CConfig.SQLITE_AFF_TEXT:
            assert CConfig.MEM_Str == (CConfig.MEM_Blob>>3)
            flags |= (flags & CConfig.MEM_Blob)>>3;
            self.set_flags(flags)
            self.applyAffinity(CConfig.SQLITE_AFF_TEXT, encoding);
            assert self.get_flags() & CConfig.MEM_Str or rffi.getintfield(self.pMem.db, 'mallocFailed')
            flags = self.get_flags()
            flags &= ~(CConfig.MEM_Int|CConfig.MEM_Real|CConfig.MEM_Blob|CConfig.MEM_Zero)
            self.set_flags(flags)
        else:
            assert 0, "unknown affinity"

    def sqlite3VdbeIntegerAffinity(self):
        """
        The MEM structure is already a MEM_Real.  Try to also make it a
        MEM_Int if we can.
        """
        flags = self.get_flags()
        # this assertion doesn't make sense - this codepath is reached from
        # the applyAffinity function if CConfig.MEM_Real is NOT true
        # assert flags & CConfig.MEM_Real
        assert not flags & CConfig.MEM_RowSet
        # assert( mem->db==0 || sqlite3_mutex_held(mem->db->mutex) );
        # assert( EIGHT_BYTE_ALIGNMENT(mem) );
        floatval = self.get_u_r()
        intval = int(floatval)
        # Only mark the value as an integer if
        #
        #    (1) the round-trip conversion real->int->real is a no-op, and
        #    (2) The integer is neither the largest nor the smallest
        #        possible integer (ticket #3922)
        #
        # The second and third terms in the following conditional enforces
        # the second condition under the assumption that addition overflow causes
        # values to wrap around.
        if floatval == float(intval) and intval < sys.maxint and intval > (-sys.maxint - 1):
            self.set_u_i(intval)
            self.MemSetTypeFlag(CConfig.MEM_Int)
        return flags

    def applyAffinity(self, affinity, enc):
        """
         Processing is determine by the affinity parameter:

         SQLITE_AFF_INTEGER:
         SQLITE_AFF_REAL:
         SQLITE_AFF_NUMERIC:
            Try to convert pRec to an integer representation or a
            floating-point representation if an integer representation
            is not possible.  Note that the integer representation is
            always preferred, even if the affinity is REAL, because
            an integer representation is more space efficient on disk.

         SQLITE_AFF_TEXT:
            Convert pRec to a text representation.

         SQLITE_AFF_NONE:
            No-op.  pRec is unchanged.
        """
        flags = self.get_flags()
        assert isinstance(affinity, int)
        if affinity >= CConfig.SQLITE_AFF_NUMERIC:
            assert affinity in (CConfig.SQLITE_AFF_INTEGER,
                                CConfig.SQLITE_AFF_REAL,
                                CConfig.SQLITE_AFF_NUMERIC)
            if flags & CConfig.MEM_Int == 0:
                if flags & CConfig.MEM_Real == 0:
                    if flags & CConfig.MEM_Str:
                        self.applyNumericAffinity(1)
                    else:
                        self.sqlite3VdbeIntegerAffinity()
        elif affinity == CConfig.SQLITE_AFF_TEXT:
            # Only attempt the conversion to TEXT if there is an integer or real
            # representation (blob and NULL do not get converted) but no string
            # representation.

            if not (flags & CConfig.MEM_Str) and flags & (CConfig.MEM_Real|CConfig.MEM_Int):
                self.sqlite3VdbeMemStringify(enc, 1)

    def sqlite3VdbeMemStringify(self, enc, bForce):
        """
        Add MEM_Str to the set of representations for the given Mem.  Numbers
        are converted using sqlite3_snprintf().  Converting a BLOB to a string
        is a no-op.

        Existing representations MEM_Int and MEM_Real are invalidated if
        bForce is true but are retained if bForce is false.

        A MEM_Null value will never be passed to this function. This function is
        used for converting values to text for returning to the user (i.e. via
        sqlite3_value_text()), or for ensuring that values to be used as btree
        keys are strings. In the former case a NULL pointer is returned the
        user and the latter is an internal programming error.
        """
        self.invalidate_cache()
        return capi.sqlite3_sqlite3VdbeMemStringify(self.pMem, enc, bForce)

    def sqlite3VdbeMemNulTerminate(self):
        """
        Make sure the given Mem is \u0000 terminated.
        """
        if (self.get_flags() & (CConfig.MEM_Term | CConfig.MEM_Str)) != CConfig.MEM_Str:
            return 0
        self.invalidate_cache()
        return rffi.cast(
            lltype.Signed, capi.sqlite3_sqlite3VdbeMemNulTerminate(self.pMem))

    def sqlite3VdbeMemIntegerify(self):
        """
        Convert pMem to type integer.  Invalidate any prior representations.
        """
        flags = self.get_flags()
        if not flags & CConfig.MEM_Int:
            # only set u.i if not already an Int
            self.set_u_i(self.sqlite3VdbeIntValue())
        self.MemSetTypeFlag(CConfig.MEM_Int)
        return CConfig.SQLITE_OK

    def sqlite3VdbeMemRealify(self):
        self.set_u_r(self.sqlite3VdbeRealValue())
        self.MemSetTypeFlag(CConfig.MEM_Real)

    def sqlite3VdbeMemNumerify(self):
        """
        Convert pMem so that it has types MEM_Real or MEM_Int or both.
        Invalidate any prior representations.

        Every effort is made to force the conversion, even if the input
        is a string that does not look completely like a number.  Convert
        as much of the string as we can and ignore the rest.
        """
        flags = self.get_flags()
        if not flags & (CConfig.MEM_Int|CConfig.MEM_Real|CConfig.MEM_Null):
            assert flags & (CConfig.MEM_Blob|CConfig.MEM_Str)
            rc, value = self._atoi()
            if rc == 0:
                self.set_u_i(value)
                self.MemSetTypeFlag(CConfig.MEM_Int)
            else:
                self.set_u_r(self.sqlite3VdbeRealValue())
                self.MemSetTypeFlag(CConfig.MEM_Real)
                self.sqlite3VdbeIntegerAffinity()
        assert flags & (CConfig.MEM_Int|CConfig.MEM_Real|CConfig.MEM_Null)
        self.set_flags(flags & ~(CConfig.MEM_Str|CConfig.MEM_Blob))
        return CConfig.SQLITE_OK

    def applyNumericAffinity(self, bTryForInt):
        """
        Try to convert a value into a numeric representation if we can
        do so without loss of information.  In other words, if the string
        looks like a number, convert it into a number.  If it does not
        look like a number, leave it alone.

        If the bTryForInt flag is true, then extra effort is made to give
        an integer representation.  Strings that look like floating point
        values but which have no fractional component (example: '48.00')
        will have a MEM_Int representation when bTryForInt is true.

        If bTryForInt is false, then if the input string contains a decimal
        point or exponential notation, the result is only MEM_Real, even
        if there is an exact integer representation of the quantity.
        """
        # use the C function for now
        self.invalidate_cache()
        capi.sqlite3_applyNumericAffinity(self.pMem, bTryForInt)

    def numericType(self):
        """
        Return the numeric type for pMem, either MEM_Int or MEM_Real or both or
        none.

        Unlike applyNumericAffinity(), this routine does not modify pMem->flags.
        But it does set pMem->u.r and pMem->u.i appropriately.
        """
        flags = self.get_flags()
        if flags & (CConfig.MEM_Int | CConfig.MEM_Real):
            return flags & (CConfig.MEM_Int | CConfig.MEM_Real)
        if flags & (CConfig.MEM_Str | CConfig.MEM_Blob):
            val1 = lltype.malloc(rffi.DOUBLEP.TO, 1, flavor='raw')
            val1[0] = 0.0
            atof = capi.sqlite3AtoF(self.get_z(), val1, self.get_n(), self.get_enc())
            self.set_u_r(val1[0])
            lltype.free(val1, flavor='raw')

            if atof == 0:
                return 0
            atoi64, value = self._atoi()
            self.set_u_i(value)

            if atoi64 == CConfig.SQLITE_OK:
                return CConfig.MEM_Int

            return CConfig.MEM_Real
        return 0

    def sqlite3VdbeMemGrow(self, n, bPreserve):
        self.invalidate_cache()
        return rffi.cast(
            lltype.Signed,
            capi.sqlite3VdbeMemGrow(
                self.pMem, rffi.cast(rffi.INT, n), rffi.cast(rffi.INT, bPreserve)))

    def sqlite3VdbeMemRelease(self):
        self.VdbeMemRelease()
        if self.VdbeMemDynamic() or self.get_szMalloc():
            self.vdbeMemClear()

    def vdbeMemClear(self):
        if self.VdbeMemDynamic():
            self.vdbeMemClearExternAndSetNull()
        if self.get_szMalloc():
            capi.sqlite3DbFree(self.hlquery.db, rffi.cast(rffi.VOIDP, self.get_zMalloc()))
            self.set_zMalloc_null()
        self.set_z_null()

    def vdbeMemClearExternAndSetNull(self):
        capi.vdbeMemClearExternAndSetNull(self.pMem)
        self.invalidate_cache()

    def sqlite3VdbeMemSetInt64(self, val, constant=False):
        if self.get_flags() != CConfig.MEM_Int:
            self.sqlite3VdbeMemRelease()
            self.set_flags(CConfig.MEM_Int)
        self.set_u_i(val, constant=constant)

    def sqlite3VdbeMemSetDouble(self, val):
        """
        Delete any previous value and set the value stored in *pMem to val,
        manifest type REAL.
        """
        if math.isnan(val):
            self.sqlite3VdbeMemSetNull();
        else:
            if self.get_flags() != CConfig.MEM_Real:
                self.sqlite3VdbeMemRelease()
                self.set_flags(CConfig.MEM_Real)
            self.set_u_r(val)

    def sqlite3VdbeMemSetNull(self):
        """ Delete any previous value and set the value stored in *pMem to NULL. """
        if self.get_flags() & CConfig.MEM_Frame or self.get_flags() & CConfig.MEM_RowSet:
            self.invalidate_cache()
            capi.sqlite3VdbeMemSetNull(self.pMem)
        else:
            # fast path
            self.MemSetTypeFlag(CConfig.MEM_Null)


    def MemSetTypeFlag(self, flags):
        self.set_flags((self.get_flags() & ~(CConfig.MEM_TypeMask | CConfig.MEM_Zero)) | flags)

    def VdbeMemDynamic(self):
        return (self.get_flags() & (CConfig.MEM_Agg|CConfig.MEM_Dyn|CConfig.MEM_RowSet|CConfig.MEM_Frame))!=0

    def VdbeMemRelease(self):
        if self.VdbeMemDynamic():
            self.vdbeMemClearExternAndSetNull()


    def sqlite3VdbeIntValue(self):
        """
        Return some kind of integer value which is the best we can do
        at representing the value that *pMem describes as an integer.
        If pMem is an integer, then the value is exact.  If pMem is
        a floating-point then the value returned is the integer part.
        If pMem is a string or blob, then we make an attempt to convert
        it into a integer and return that.  If pMem represents an
        an SQL-NULL value, return 0.

        If pMem represents a string value, its encoding might be changed.
        """

        flags = self.get_flags()
        #assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
        #assert( EIGHT_BYTE_ALIGNMENT(pMem) );
        if flags & CConfig.MEM_Int:
            return self.get_u_i()
        elif flags & CConfig.MEM_Real:
            return int(self.get_u_r())
        elif flags & (CConfig.MEM_Str|CConfig.MEM_Blob):
            _, value = self._atoi()
            return value
        else:
            return 0

    def sqlite3VdbeRealValue(self):
        """
        Return the best representation of pMem that we can get into a
        double.  If pMem is already a double or an integer, return its
        value.  If it is a string or blob, try to convert it to a double.
        If it is a NULL, return 0.0.
        """
        flags = self.get_flags()
        # assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
        # assert( EIGHT_BYTE_ALIGNMENT(pMem) );
        if flags & CConfig.MEM_Real:
            return self.get_u_r()
        elif flags & CConfig.MEM_Int:
            return self.get_u_i()
        elif flags & (CConfig.MEM_Str | CConfig.MEM_Blob):
            val = lltype.malloc(rffi.DOUBLEP.TO, 1, flavor='raw')
            val[0] = 0.0
            capi.sqlite3AtoF(self.get_z(), val, self.get_n(), self.get_enc())
            ret = val[0]
            lltype.free(val, flavor='raw')
            return ret
        else:
            return 0.0

    def memIsValid(self):
        return not (self.get_flags() & CConfig.MEM_Undefined)

    def ExpandBlob(self):
        if self.get_flags() & CConfig.MEM_Zero:
            return self.sqlite3VdbeMemExpandBlob()
        return 0

    def sqlite3VdbeMemExpandBlob(self):
        self.invalidate_cache()
        return rffi.cast(lltype.Signed, capi.sqlite3VdbeMemExpandBlob(self.pMem))

    # _______________________________________________________________
    # methods induced by sqlite3 functions below


    def sqlite3VdbeMemShallowCopy(self, from_, srcType):
        """
        Make an shallow copy of pFrom into pTo.  Prior contents of
        pTo are freed.  The pFrom->z field is not duplicated.  If
        pFrom->z is used, then pTo->z points to the same thing as pFrom->z
        and flags gets srcType (either MEM_Ephem or MEM_Static).
        """
        assert not from_.get_flags() & CConfig.MEM_RowSet
        if self.VdbeMemDynamic():
            self.vdbeMemClearExternAndSetNull()
        fromflags = from_.get_flags()
        if fromflags & CConfig.MEM_Real:
            self.set_u_r(from_.get_u_r())
        if fromflags & CConfig.MEM_Int:
            self.set_u_i(from_.get_u_i())
        if fromflags & CConfig.MEM_Zero:
            self.set_u_nZero(from_.get_u_nZero())
        if fromflags & CConfig.MEM_Agg:
            assert fromflags == CConfig.MEM_Agg
            self.set_u_pDef(from_.get_u_pDef())
        if fromflags & CConfig.MEM_RowSet:
            assert fromflags == CConfig.MEM_RowSet
            self.set_u_pRowSet(from_.get_u_pRowSet())
        if fromflags & CConfig.MEM_Frame:
            assert fromflags == CConfig.MEM_Frame
            self.set_u_pFrame(from_.get_u_pFrame())
        self.set_enc(from_.get_enc())
        self.set_n(from_.get_n())
        self.set_z(from_.get_z())
        if not fromflags & CConfig.MEM_Static:
            fromflags &= ~(CConfig.MEM_Dyn | CConfig.MEM_Static | CConfig.MEM_Ephem)
            assert srcType == CConfig.MEM_Ephem or srcType == CConfig.MEM_Static
            fromflags |= srcType
        self.set_flags(fromflags)


    @jit.dont_look_inside
    def _memcpy_partial_hidden(self, from_):
        MEMCELLSIZE = rffi.offsetof(capi.MEM, 'zMalloc')
        rffi.c_memcpy(rffi.cast(rffi.VOIDP, self.pMem), rffi.cast(rffi.VOIDP, from_.pMem), MEMCELLSIZE)

    def sqlite3MemCompare(self, other, coll):
        flags1 = self.get_flags()
        flags2 = other.get_flags()
        combined_flags = flags1 | flags2
        if combined_flags & CConfig.MEM_Null:
            return rarithmetic.intmask(flags2 & CConfig.MEM_Null) - rarithmetic.intmask(flags1 & CConfig.MEM_Null)

        # If one value is a number and the other is not, the number is less.
        # If both are numbers, compare as reals if one is a real, or as integers
        # if both values are integers.
        if (flags1 | flags2) & (CConfig.MEM_Int | CConfig.MEM_Real):
            # both are ints
            if flags1 & flags2 & CConfig.MEM_Int:
                i1 = self.get_u_i()
                i2 = other.get_u_i()
                if i1 < i2:
                    return -1
                if i1 > i2:
                    return 1
                return 0
            else:
                if flags1 & CConfig.MEM_Real:
                    r1 = self.get_u_r()
                elif flags1 & CConfig.MEM_Int:
                    r1 = float(self.get_u_i())
                else:
                    return 1
                if flags2 & CConfig.MEM_Real:
                    r2 = other.get_u_r()
                elif flags2 & CConfig.MEM_Int:
                    r2 = float(other.get_u_i())
                else:
                    return 1
                if r1 < r2:
                    return -1
                if r1 > r2:
                    return 1
                return 0
        self.invalidate_cache()
        other.invalidate_cache()
        return capi.sqlite3_sqlite3MemCompare(self.pMem, other.pMem, coll)

    def sqlite3VdbeSerialType(self, file_format):
        """ Return the serial-type for the value stored in pMem and the length
        of the data """
        return self.sqlite3VdbeSerialType_Len_and_HdrLen(file_format)[0]

    def sqlite3VdbeSerialType_Len_and_HdrLen(self, file_format):
        from sqpyte.translated import sqlite3VarintLen
        # length info from: sqlite3VdbeSerialTypeLen

        flags = self.get_flags()
        if flags & CConfig.MEM_Null:
            return _type_size_and_hdrsize(0)
        if flags & CConfig.MEM_Int:
            # Figure out whether to use 1, 2, 4, 6 or 8 bytes.
            serial_type = _get_serial_type_of_int_hidden(self.get_u_i(), file_format)
            return _type_size_and_hdrsize(serial_type)
        if flags & CConfig.MEM_Real:
          return _type_size_and_hdrsize(7)
        #assert( pMem.db.mallocFailed || flags&(MEM_Str|MEM_Blob) );
        n = self.get_n()
        if flags & CConfig.MEM_Zero:
            n += self.get_u_nZero()
        assert n >= 0
        serial_type = (n * 2) + (12 + int((flags & CConfig.MEM_Str) != 0))
        return serial_type, n, (
                1 if serial_type <= 127 else sqlite3VarintLen(serial_type))

    def sqlite3VdbeSerialPut(self, buf, serial_type):
        return capi.sqlite3VdbeSerialPut(buf, self.pMem, rffi.cast(CConfig.u32, serial_type))

    def _sqlite3VdbeSerialPut_with_length(self, buf, serial_type, length):
        flags = self.get_flags()
        if flags & CConfig.MEM_Null:
            return 0
        if flags & (CConfig.MEM_Int | CConfig.MEM_Real):
            if flags & CConfig.MEM_Int:
                i = self.get_u_i()
            else:
                i = longlong2float.float2longlong(self.get_u_r())
            _write_int_to_buf(buf, i, length)
            return length
        else:
            rffi.c_memcpy(rffi.cast(rffi.VOIDP, buf), rffi.cast(rffi.VOIDP, self.get_z()), length)
            return length

    def sqlite3VdbeChangeEncoding(self, encoding):
        #if not (self.get_flags() & CConfig.MEM_Str) or self.get_enc() == encoding:
        #    return CConfig.SQLITE_OK
        self.invalidate_cache()
        return capi.sqlite3VdbeChangeEncoding(self.pMem, encoding)

    def sqlite3VdbeMemTooBig(self):
        if not (self.get_flags() & (CConfig.MEM_Str | CConfig.MEM_Blob)):
            return 0
        return capi.sqlite3VdbeMemTooBig(self.pMem)

    def sqlite3VdbeMemMove(self, from_):
        self.sqlite3VdbeMemRelease()
        self.memcpy_full(from_)
        from_.set_flags(CConfig.MEM_Null)
        from_.set_szMalloc(0)

    def memcpy_full(self, from_):
        self.invalidate_cache()
        self._memcpy_full_hidden(from_)
        self.assure_flags(from_.get_flags())

    @jit.dont_look_inside
    def _memcpy_full_hidden(self, from_):
        rffi.c_memcpy(rffi.cast(rffi.VOIDP, self.pMem), rffi.cast(rffi.VOIDP, from_.pMem), rffi.sizeof(capi.MEM))

    def sqlite3VdbeMemMakeWriteable(self):
        """
        Make the given Mem object MEM_Dyn.  In other words, make it so
        that any TEXT or BLOB content is stored in memory obtained from
        malloc().  In this way, we know that the memory is safe to be
        overwritten or altered.

        Return SQLITE_OK on success or SQLITE_NOMEM if malloc fails.
        """
        assert not self.get_flags() & CConfig.MEM_RowSet
        self.ExpandBlob()
        f = self.get_flags()
        z = self.get_z()
        if f & (CConfig.MEM_Str | CConfig.MEM_Blob) and z != self.get_zMalloc():
            if self.sqlite3VdbeMemGrow(self.get_n() + 2, 1):
                return CConfig.SQLITE_NOMEM
            z = self.get_z()
            n = self.get_n()
            z[n] = chr(0)
            z[n + 1] = chr(0)
            self.set_flags(f | CConfig.MEM_Term)
        return CConfig.SQLITE_OK


    # public API functions

    def sqlite3_value_type(self):
        return _type_encoding_list[self.get_flags(promote=True) & CConfig.MEM_AffMask]

    def sqlite3_value_numeric_type(self):
        """
        Try to convert the type of a function argument or a result column
        into a numeric representation.  Use either INTEGER or REAL whichever
        is appropriate.  But only do the conversion if it is possible without
        loss of information and return the revised type of the argument.
        """
        eType = self.sqlite3_value_type()
        if eType == CConfig.SQLITE_TEXT:
            self.applyNumericAffinity(0)
            eType = self.sqlite3_value_type()
        return eType

    sqlite3_value_int64 = sqlite3VdbeIntValue
    sqlite3_value_double = sqlite3VdbeRealValue

    def sqlite3_value_text(self):
        return self.sqlite3ValueText(CConfig.SQLITE_UTF8)


    def sqlite3ValueText(self, enc):
        """
        This function is only available internally, it is not part of the
        external API. It works in a similar way to sqlite3_value_text(),
        except the data returned is in the encoding specified by the second
        parameter, which must be one of SQLITE_UTF16BE, SQLITE_UTF16LE or
        SQLITE_UTF8.

         (2006-02-16:)  The enc value can be or-ed with SQLITE_UTF16_ALIGNED.
         If that is the case, then the result must be aligned on an even byte
         boundary.
        """
        flags = self.get_flags()
        assert (flags & CConfig.MEM_RowSet) == 0
        if ((flags & (CConfig.MEM_Str|CConfig.MEM_Term)) ==
                (CConfig.MEM_Str|CConfig.MEM_Term) and
                self.get_enc_signed() == rffi.cast(lltype.Signed, enc)):
            return self.get_z()
        if flags & CConfig.MEM_Null:
            return lltype.nullptr(rffi.CCHARP.TO)
        return rffi.cast(rffi.CCHARP, self.valueToText(enc))


    def valueToText(self, enc):
        """
        The pVal argument is known to be a value other than NULL.
        Convert it into a string with encoding enc and return a pointer
        to a zero-terminated version of that string.
        """
        self.invalidate_cache()
        return capi.valueToText(self.pMem, enc)

    def sqlite3ValueBytes(self, enc):
        """
        Return the number of bytes in the sqlite3_value object assuming
        that it uses the encoding "enc"
        """
        if self.get_flags() & CConfig.MEM_Blob or self.sqlite3ValueText(enc):
            if self.get_flags() & CConfig.MEM_Zero:
                return self.get_n() + self.get_u_nZero()
            else:
                return self.get_n()
        return 0

    def sqlite3_value_bytes(self):
        return self.sqlite3ValueBytes(CConfig.SQLITE_UTF8)

    sqlite3_result_int64 = sqlite3VdbeMemSetInt64
    sqlite3_result_double = sqlite3VdbeMemSetDouble
    sqlite3_result_null = sqlite3VdbeMemSetNull

    @jit.dont_look_inside
    def sqlite3_result_text(self, s):
        self.invalidate_cache()
        with rffi.scoped_str2charp(s) as charp:
            capi.sqlite3VdbeMemSetStr(
                self.pMem, charp, len(s),
                CConfig.SQLITE_UTF8,
                rffi.cast(rffi.VOIDP, CConfig.SQLITE_TRANSIENT))


@jit.look_inside_iff(lambda buf, v, length: jit.isconstant(length))
def _write_int_to_buf(buf, v, length):
    v = rarithmetic.r_uint(v)
    while length:
        length -= 1
        buf[length] = rffi.cast(rffi.UCHAR, v & 0xff)
        v >>= 8

@jit.look_inside_iff(lambda i, file_format: jit.isconstant(i))
def _get_serial_type_of_int_hidden(i, file_format):
    MAX_6BYTE = (0x00008000 << 32) - 1
    if i < 0:
        # test prevents:  u = -(-9223372036854775808)
        if i < -MAX_6BYTE:
            return 6
        u = rarithmetic.r_uint(-i)
    else:
        u = rarithmetic.r_uint(i)
    if u <= 127:
        if rffi.cast(lltype.Signed, file_format) > 4:
            if i == 0:
                return 8
            elif i == 1:
                return 9
        return 1
    if u <= 32767:
        return 2
    if u <= 8388607:
        return 3
    if u<=2147483647:
        return 4
    if u <= MAX_6BYTE:
        return 5
    return 6

def _type_size_and_hdrsize(serial_type):
    return serial_type, SIZE_INFO_OF_SERIAL_TYPES[serial_type], 1
SIZE_INFO_OF_SERIAL_TYPES = [0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0]

_type_encoding_list = [
     CConfig.SQLITE_BLOB,     # 0x00
     CConfig.SQLITE_NULL,     # 0x01
     CConfig.SQLITE_TEXT,     # 0x02
     CConfig.SQLITE_NULL,     # 0x03
     CConfig.SQLITE_INTEGER,  # 0x04
     CConfig.SQLITE_NULL,     # 0x05
     CConfig.SQLITE_INTEGER,  # 0x06
     CConfig.SQLITE_NULL,     # 0x07
     CConfig.SQLITE_FLOAT,    # 0x08
     CConfig.SQLITE_NULL,     # 0x09
     CConfig.SQLITE_FLOAT,    # 0x0a
     CConfig.SQLITE_NULL,     # 0x0b
     CConfig.SQLITE_INTEGER,  # 0x0c
     CConfig.SQLITE_NULL,     # 0x0d
     CConfig.SQLITE_INTEGER,  # 0x0e
     CConfig.SQLITE_NULL,     # 0x0f
     CConfig.SQLITE_BLOB,     # 0x10
     CConfig.SQLITE_NULL,     # 0x11
     CConfig.SQLITE_TEXT,     # 0x12
     CConfig.SQLITE_NULL,     # 0x13
     CConfig.SQLITE_INTEGER,  # 0x14
     CConfig.SQLITE_NULL,     # 0x15
     CConfig.SQLITE_INTEGER,  # 0x16
     CConfig.SQLITE_NULL,     # 0x17
     CConfig.SQLITE_FLOAT,    # 0x18
     CConfig.SQLITE_NULL,     # 0x19
     CConfig.SQLITE_FLOAT,    # 0x1a
     CConfig.SQLITE_NULL,     # 0x1b
     CConfig.SQLITE_INTEGER,  # 0x1c
     CConfig.SQLITE_NULL,     # 0x1d
     CConfig.SQLITE_INTEGER,  # 0x1e
     CConfig.SQLITE_NULL,     # 0x1f
]

class Virt(object):
    def __init__(self, cs):
        self.cs = cs

class CacheHolder(object):
    _immutable_fields_ = ['integers', 'floats', '_invalid_cache_state', 'use_cache']

    def __init__(self, num_flags, use_cache=True):
        self._invalid_cache_state = all_unknown(num_flags)
        self._nonvirt_cache_state = self._invalid_cache_state
        self._virt_cache_state = None
        self.integers = [0] * num_flags
        self.floats = [0.0] * num_flags
        self.use_cache = use_cache

    def cache_state(self):
        if self._virt_cache_state is not None:
            return jit.promote(self._virt_cache_state.cs)
        else:
            return jit.promote(self._nonvirt_cache_state)

    def set_cache_state(self, cache_state):
        if not self.use_cache:
            cache_state = self._invalid_cache_state
        if self._virt_cache_state is not None:
            self._virt_cache_state = Virt(cache_state)
        else:
            self._nonvirt_cache_state = cache_state

    def prepare_return(self):
        if not jit.we_are_jitted():
            assert self._nonvirt_cache_state is None
        self._nonvirt_cache_state = self.cache_state()
        self._virt_cache_state = None

    def reenter(self):
        if not jit.we_are_jitted():
            assert self._virt_cache_state is None
        cache_state = self._nonvirt_cache_state
        self._nonvirt_cache_state = None
        self._virt_cache_state = Virt(cache_state)
        return cache_state

    def hide(self):
        self._virt_cache_state = None

    def reveal(self, x):
        self._virt_cache_state = Virt(x)

    def invalidate(self, i):
        self.set_cache_state(self.cache_state().set_unknown(i))

    def invalidate_all(self):
        self.set_cache_state(self._invalid_cache_state)

    def invalidate_all_outside(self):
        if not jit.we_are_jitted():
            assert self._virt_cache_state is None
        self._nonvirt_cache_state = self._invalid_cache_state

    def get_flags(self, mem):
        i = mem._cache_index
        if i == -1:
            return rffi.cast(lltype.Unsigned, mem.pMem.flags)
        state = self.cache_state()
        if state.is_flag_known(i):
            if not jit.we_are_jitted() and mem.pMem:
                assert state.get_flags(i) == rffi.cast(lltype.Unsigned, mem.pMem.flags)
            return state.get_flags(i)
        flags = rffi.cast(lltype.Unsigned, mem.pMem.flags)
        self.set_cache_state(state.change_flags(i, flags))
        return flags

    def set_flags(self, mem, newflags):
        self._set_flags(mem, newflags, needs_write=True)

    def assure_flags(self, mem, newflags):
        self._set_flags(mem, newflags, needs_write=False)

    def _set_flags(self, mem, newflags, needs_write):
        i = mem._cache_index
        if i == -1:
            if needs_write:
                rffi.setintfield(mem.pMem, 'flags', newflags)
            return
        state = self.cache_state()
        if state.is_flag_known(i) and state.get_flags(i) == newflags:
            return
        if needs_write:
            rffi.setintfield(mem.pMem, 'flags', newflags)
        if not self.use_cache:
            return
        self.set_cache_state(state.change_flags(i, newflags))
        if not jit.we_are_jitted() and mem.pMem:
            assert self.cache_state().get_flags(i) == rffi.cast(lltype.Unsigned, mem.pMem.flags) == newflags


    def get_u_i(self, mem):
        i = mem._cache_index
        if i == -1:
            return mem.pMem.u.i
        state = self.cache_state()
        if state.is_constant_u_i(i):
            return state.get_constant_u_i(i)
        if state.is_u_i_known(i):
            return self.integers[i]
        u_i = mem.pMem.u.i
        return u_i
        self.integers[i] = u_i
        self.set_cache_state(state.add_knowledge(i, STATE_INT_KNOWN))
        return u_i

    def get_u_r(self, mem):
        i = mem._cache_index
        if i == -1:
            return mem.pMem.u.r
        state = self.cache_state()
        if state.is_constant_u_r(i):
            return state.get_constant_u_r(i)
        if state.is_u_r_known(i):
            return self.floats[i]
        u_r = mem.pMem.u.r
        return u_r
        self.floats[i] = u_r
        self.set_cache_state(state.add_knowledge(i, STATE_FLOAT_KNOWN))
        return u_r

    def set_u_i(self, mem, u_i, constant=False):
        i = mem._cache_index
        mem.pMem.u.i = u_i
        if i == -1:
            return
        state = self.cache_state()
        if not constant:
            status = (state.cache_states[i] & ~STATE_CONSTANT) & ~STATE_INT_KNOWN
            self.set_cache_state(state.change_cache_state(i, status))
            return
            self.integers[i] = u_i
        else:
            self.set_cache_state(state.set_u_i_constant(i, u_i))

    def set_u_r(self, mem, u_r, constant=False):
        i = mem._cache_index
        mem.pMem.u.r = u_r
        if i == -1:
            return
        state = self.cache_state()
        if not constant:
            status = (state.cache_states[i] & ~STATE_CONSTANT) & ~STATE_FLOAT_KNOWN
            self.set_cache_state(state.change_cache_state(i, status))
            return
            self.floats[i] = u_r
        else:
            self.set_cache_state(state.set_u_r_constant(i, u_r))

    def is_constant_u_i(self, mem):
        i = mem._cache_index
        if i == -1:
            return False
        return self.cache_state().is_constant_u_i(i)

    def is_constant_u_r(self, mem):
        i = mem._cache_index
        if i == -1:
            return False
        return self.cache_state().is_constant_u_r(i)


STATE_UNKNOWN = 0
STATE_FLAG_KNOWN = 1
STATE_INT_KNOWN = 2
STATE_FLOAT_KNOWN = 4
STATE_CONSTANT = 8

def state_eq(self, other):
    return self.eq(other)

def state_hash(self):
    return self.hash()

class CacheState(object):
    _immutable_fields_ = ['all_flags[*]', 'cache_states[*]', 'u_i_constants[*]', 'u_r_constants[*]']
    _cache = objectmodel.r_dict(state_eq, state_hash)

    def __init__(self, all_flags, cache_states, u_i_constants, u_r_constants):
        self.all_flags = all_flags
        self.cache_states = cache_states
        self.u_i_constants = u_i_constants
        self.u_r_constants = u_r_constants

    def copy(self):
        return CacheState(self.all_flags[:], self.cache_states[:], self.u_i_constants[:], self.u_r_constants[:])

    def repr(self):
        return "CacheState(%s, %s, %s, %s)" % (self.all_flags, self.cache_states, self.u_i_constants, self.u_r_constants)
    __repr__ = repr

    def eq(self, other):
        return (self.all_flags == other.all_flags and
                self.cache_states == other.cache_states and
                self.u_i_constants == other.u_i_constants and
                self.u_r_constants == other.u_r_constants
                )


    def hash(self):
        from rpython.rlib.rarithmetic import intmask
        x = 0x345678
        for item in self.all_flags:
            y = rffi.cast(lltype.Signed, item)
            x = intmask((1000003 * x) ^ y)
        for item in self.cache_states:
            y = rffi.cast(lltype.Signed, item)
            x = intmask((1000003 * x) ^ y)
        for item in self.u_i_constants:
            y = rffi.cast(lltype.Signed, item)
            x = intmask((1000003 * x) ^ y)
        for item in self.u_r_constants:
            y = rffi.cast(lltype.Signed, item)
            x = intmask((1000003 * x) ^ y)
        return x

    def unique(self):
        newself = self._cache.get(self, None)
        if newself:
            assert newself.all_flags == self.all_flags
            assert newself.cache_states == self.cache_states
            assert newself.u_i_constants == self.u_i_constants
            assert newself.u_r_constants == self.u_r_constants
            return newself
        self._cache[self] = self
        return self

    @jit.elidable_promote('all')
    def change_flags(self, i, new_flags):
        if self.is_flag_known(i) and self.all_flags[i] == new_flags:
            return self
        self = self.add_knowledge(i, STATE_FLAG_KNOWN)
        result = self.copy()
        result.all_flags[i] = new_flags
        return result.unique()

    def add_knowledge(self, i, statusbits):
        status = self.cache_states[i] | statusbits
        return self.change_cache_state(i, status)

    @jit.elidable_promote('all')
    def change_cache_state(self, i, status):
        if self.cache_states[i] == status:
            return self
        result = self.copy()
        result.cache_states[i] = status
        return result.unique()

    @jit.elidable_promote('all')
    def set_u_i_constant(self, i, u_i):
        self = self.add_knowledge(i, STATE_INT_KNOWN | STATE_CONSTANT)
        result = self.copy()
        result.u_i_constants[i] = u_i
        return result.unique()

    @jit.elidable_promote('all')
    def set_u_r_constant(self, i, u_r):
        self = self.add_knowledge(i, STATE_FLOAT_KNOWN | STATE_CONSTANT)
        result = self.copy()
        result.u_r_constants[i] = u_r
        return result.unique()

    def set_unknown(self, i):
        if self.cache_states[i]:
            return self.change_flags(i, 0).change_cache_state(i, STATE_UNKNOWN)
        return self

    def is_flag_known(self, i):
        return bool(self.cache_states[i] & STATE_FLAG_KNOWN)

    def is_u_i_known(self, i):
        return bool(self.cache_states[i] & STATE_INT_KNOWN)

    def is_u_r_known(self, i):
        return bool(self.cache_states[i] & STATE_FLOAT_KNOWN)

    def is_constant_u_i(self, i):
        return self.is_u_i_known(i) and bool(self.cache_states[i] & STATE_CONSTANT)

    def is_constant_u_r(self, i):
        return self.is_u_r_known(i) and bool(self.cache_states[i] & STATE_CONSTANT)

    def get_flags(self, i):
        assert self.is_flag_known(i)
        return self.all_flags[i]

    def get_constant_u_i(self, i):
        assert self.is_constant_u_i(i)
        return self.u_i_constants[i]

    def get_constant_u_r(self, i):
        assert self.is_constant_u_r(i)
        return self.u_r_constants[i]

def all_unknown(num_flags):
    return CacheState([0] * num_flags, [0] * num_flags, [0] * num_flags, [0] * num_flags)
