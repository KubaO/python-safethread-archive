"""
Read and write ZIP files.

XXX references to utf-8 need further investigation.
"""
import struct, os, time, sys, shutil
import binascii, io

try:
    import zlib # We may need its compression method
    crc32 = zlib.crc32
except ImportError:
    zlib = None
    crc32 = binascii.crc32

__all__ = ["BadZipfile", "error", "ZIP_STORED", "ZIP_DEFLATED", "is_zipfile",
           "ZipInfo", "ZipFile", "PyZipFile", "LargeZipFile" ]

class BadZipfile(Exception):
    pass


class LargeZipFile(Exception):
    """
    Raised when writing a zipfile, the zipfile requires ZIP64 extensions
    and those extensions are disabled.
    """

error = BadZipfile      # The exception raised by this module

ZIP64_LIMIT= (1 << 31) - 1

# constants for Zip file compression methods
ZIP_STORED = 0
ZIP_DEFLATED = 8
# Other ZIP compression methods not supported

# Here are some struct module formats for reading headers
structEndArchive = "<4s4H2LH"     # 9 items, end of archive, 22 bytes
stringEndArchive = b"PK\005\006"   # magic number for end of archive record
structCentralDir = "<4s4B4HLLL5HLL"# 19 items, central directory, 46 bytes
stringCentralDir = b"PK\001\002"   # magic number for central directory
structFileHeader = "<4s2B4HLLL2H"  # 12 items, file header record, 30 bytes
stringFileHeader = b"PK\003\004"   # magic number for file header
structEndArchive64Locator = "<4sLQL" # 4 items, locate Zip64 header, 20 bytes
stringEndArchive64Locator = b"PK\x06\x07" # magic token for locator header
structEndArchive64 = "<4sQHHLLQQQQ" # 10 items, end of archive (Zip64), 56 bytes
stringEndArchive64 = b"PK\x06\x06" # magic token for Zip64 header


# indexes of entries in the central directory structure
_CD_SIGNATURE = 0
_CD_CREATE_VERSION = 1
_CD_CREATE_SYSTEM = 2
_CD_EXTRACT_VERSION = 3
_CD_EXTRACT_SYSTEM = 4                  # is this meaningful?
_CD_FLAG_BITS = 5
_CD_COMPRESS_TYPE = 6
_CD_TIME = 7
_CD_DATE = 8
_CD_CRC = 9
_CD_COMPRESSED_SIZE = 10
_CD_UNCOMPRESSED_SIZE = 11
_CD_FILENAME_LENGTH = 12
_CD_EXTRA_FIELD_LENGTH = 13
_CD_COMMENT_LENGTH = 14
_CD_DISK_NUMBER_START = 15
_CD_INTERNAL_FILE_ATTRIBUTES = 16
_CD_EXTERNAL_FILE_ATTRIBUTES = 17
_CD_LOCAL_HEADER_OFFSET = 18

# indexes of entries in the local file header structure
_FH_SIGNATURE = 0
_FH_EXTRACT_VERSION = 1
_FH_EXTRACT_SYSTEM = 2                  # is this meaningful?
_FH_GENERAL_PURPOSE_FLAG_BITS = 3
_FH_COMPRESSION_METHOD = 4
_FH_LAST_MOD_TIME = 5
_FH_LAST_MOD_DATE = 6
_FH_CRC = 7
_FH_COMPRESSED_SIZE = 8
_FH_UNCOMPRESSED_SIZE = 9
_FH_FILENAME_LENGTH = 10
_FH_EXTRA_FIELD_LENGTH = 11

def is_zipfile(filename):
    """Quickly see if file is a ZIP file by checking the magic number."""
    try:
        fpin = io.open(filename, "rb")
        endrec = _EndRecData(fpin)
        fpin.close()
        if endrec:
            return True                 # file has correct magic number
    except IOError:
        pass
    return False

def _EndRecData64(fpin, offset, endrec):
    """
    Read the ZIP64 end-of-archive records and use that to update endrec
    """
    locatorSize = struct.calcsize(structEndArchive64Locator)
    fpin.seek(offset - locatorSize, 2)
    data = fpin.read(locatorSize)
    sig, diskno, reloff, disks = struct.unpack(structEndArchive64Locator, data)
    if sig != stringEndArchive64Locator:
        return endrec

    if diskno != 0 or disks != 1:
        raise BadZipfile("zipfiles that span multiple disks are not supported")

    # Assume no 'zip64 extensible data'
    endArchiveSize = struct.calcsize(structEndArchive64)
    fpin.seek(offset - locatorSize - endArchiveSize, 2)
    data = fpin.read(endArchiveSize)
    sig, sz, create_version, read_version, disk_num, disk_dir, \
            dircount, dircount2, dirsize, diroffset = \
            struct.unpack(structEndArchive64, data)
    if sig != stringEndArchive64:
        return endrec

    # Update the original endrec using data from the ZIP64 record
    endrec[1] = disk_num
    endrec[2] = disk_dir
    endrec[3] = dircount
    endrec[4] = dircount2
    endrec[5] = dirsize
    endrec[6] = diroffset
    return endrec


def _EndRecData(fpin):
    """Return data from the "End of Central Directory" record, or None.

    The data is a list of the nine items in the ZIP "End of central dir"
    record followed by a tenth item, the file seek offset of this record."""
    fpin.seek(-22, 2)               # Assume no archive comment.
    filesize = fpin.tell() + 22     # Get file size
    data = fpin.read()
    if data[0:4] == stringEndArchive and data[-2:] == b"\000\000":
        endrec = struct.unpack(structEndArchive, data)
        endrec = list(endrec)
        endrec.append("")               # Append the archive comment
        endrec.append(filesize - 22)    # Append the record start offset
        if endrec[-4] == 0xffffffff:
            return _EndRecData64(fpin, -22, endrec)
        return endrec
    # Search the last END_BLOCK bytes of the file for the record signature.
    # The comment is appended to the ZIP file and has a 16 bit length.
    # So the comment may be up to 64K long.  We limit the search for the
    # signature to a few Kbytes at the end of the file for efficiency.
    # also, the signature must not appear in the comment.
    END_BLOCK = min(filesize, 1024 * 4)
    fpin.seek(filesize - END_BLOCK, 0)
    data = fpin.read()
    start = data.rfind(stringEndArchive)
    if start >= 0:     # Correct signature string was found
        endrec = struct.unpack(structEndArchive, data[start:start+22])
        endrec = list(endrec)
        comment = data[start+22:]
        if endrec[7] == len(comment):     # Comment length checks out
            # Append the archive comment and start offset
            endrec.append(comment)
            endrec.append(filesize - END_BLOCK + start)
            if endrec[-4] == 0xffffffff:
                return _EndRecData64(fpin, - END_BLOCK + start, endrec)
            return endrec
    return      # Error, return None


class ZipInfo (object):
    """Class with attributes describing each file in the ZIP archive."""

    __slots__ = (
            'orig_filename',
            'filename',
            'date_time',
            'compress_type',
            'comment',
            'extra',
            'create_system',
            'create_version',
            'extract_version',
            'reserved',
            'flag_bits',
            'volume',
            'internal_attr',
            'external_attr',
            'header_offset',
            'CRC',
            'compress_size',
            'file_size',
            '_raw_time',
        )

    def __init__(self, filename="NoName", date_time=(1980,1,1,0,0,0)):
        self.orig_filename = filename   # Original file name in archive

        # Terminate the file name at the first null byte.  Null bytes in file
        # names are used as tricks by viruses in archives.
        null_byte = filename.find(chr(0))
        if null_byte >= 0:
            filename = filename[0:null_byte]
        # This is used to ensure paths in generated ZIP files always use
        # forward slashes as the directory separator, as required by the
        # ZIP format specification.
        if os.sep != "/" and os.sep in filename:
            filename = filename.replace(os.sep, "/")

        self.filename = filename        # Normalized file name
        self.date_time = date_time      # year, month, day, hour, min, sec
        # Standard values:
        self.compress_type = ZIP_STORED # Type of compression for the file
        self.comment = b""              # Comment for each file
        self.extra = b""                # ZIP extra data
        if sys.platform == 'win32':
            self.create_system = 0          # System which created ZIP archive
        else:
            # Assume everything else is unix-y
            self.create_system = 3          # System which created ZIP archive
        self.create_version = 20        # Version which created ZIP archive
        self.extract_version = 20       # Version needed to extract archive
        self.reserved = 0               # Must be zero
        self.flag_bits = 0              # ZIP flag bits
        self.volume = 0                 # Volume number of file header
        self.internal_attr = 0          # Internal attributes
        self.external_attr = 0          # External file attributes
        # Other attributes are set by class ZipFile:
        # header_offset         Byte offset to the file header
        # CRC                   CRC-32 of the uncompressed file
        # compress_size         Size of the compressed file
        # file_size             Size of the uncompressed file

    def FileHeader(self):
        """Return the per-file header as a string."""
        dt = self.date_time
        dosdate = (dt[0] - 1980) << 9 | dt[1] << 5 | dt[2]
        dostime = dt[3] << 11 | dt[4] << 5 | (dt[5] // 2)
        if self.flag_bits & 0x08:
            # Set these to zero because we write them after the file data
            CRC = compress_size = file_size = 0
        else:
            CRC = self.CRC
            compress_size = self.compress_size
            file_size = self.file_size

        extra = self.extra

        if file_size > ZIP64_LIMIT or compress_size > ZIP64_LIMIT:
            # File is larger than what fits into a 4 byte integer,
            # fall back to the ZIP64 extension
            fmt = '<HHQQ'
            extra = extra + struct.pack(fmt,
                    1, struct.calcsize(fmt)-4, file_size, compress_size)
            file_size = 0xffffffff # -1
            compress_size = 0xffffffff # -1
            self.extract_version = max(45, self.extract_version)
            self.create_version = max(45, self.extract_version)

        header = struct.pack(structFileHeader, stringFileHeader,
                 self.extract_version, self.reserved, self.flag_bits,
                 self.compress_type, dostime, dosdate, CRC,
                 compress_size, file_size,
                 len(self.filename), len(extra))
        return header + self.filename.encode("utf-8") + extra

    def _decodeExtra(self):
        # Try to decode the extra field.
        extra = self.extra
        unpack = struct.unpack
        while extra:
            tp, ln = unpack('<HH', extra[:4])
            if tp == 1:
                if ln >= 24:
                    counts = unpack('<QQQ', extra[4:28])
                elif ln == 16:
                    counts = unpack('<QQ', extra[4:20])
                elif ln == 8:
                    counts = unpack('<Q', extra[4:12])
                elif ln == 0:
                    counts = ()
                else:
                    raise RuntimeError("Corrupt extra field %s"%(ln,))

                idx = 0

                # ZIP64 extension (large files and/or large archives)
                # XXX Is this correct? won't this exclude 2**32-1 byte files?
                if self.file_size in (0xffffffffffffffff, 0xffffffff):
                    self.file_size = counts[idx]
                    idx += 1

                if self.compress_size == -1 or self.compress_size == 0xFFFFFFFF:
                    self.compress_size = counts[idx]
                    idx += 1

                if self.header_offset == -1 or self.header_offset == 0xffffffff:
                    old = self.header_offset
                    self.header_offset = counts[idx]
                    idx+=1

            extra = extra[ln+4:]


class _ZipDecrypter:
    """Class to handle decryption of files stored within a ZIP archive.

    ZIP supports a password-based form of encryption. Even though known
    plaintext attacks have been found against it, it is still useful
    to be able to get data out of such a file.

    Usage:
        zd = _ZipDecrypter(mypwd)
        plain_char = zd(cypher_char)
        plain_text = map(zd, cypher_text)
    """

    def _GenerateCRCTable():
        """Generate a CRC-32 table.

        ZIP encryption uses the CRC32 one-byte primitive for scrambling some
        internal keys. We noticed that a direct implementation is faster than
        relying on binascii.crc32().
        """
        poly = 0xedb88320
        table = [0] * 256
        for i in range(256):
            crc = i
            for j in range(8):
                if crc & 1:
                    crc = ((crc >> 1) & 0x7FFFFFFF) ^ poly
                else:
                    crc = ((crc >> 1) & 0x7FFFFFFF)
            table[i] = crc
        return table
    crctable = _GenerateCRCTable()

    def _crc32(self, ch, crc):
        """Compute the CRC32 primitive on one byte."""
        return ((crc >> 8) & 0xffffff) ^ self.crctable[(crc ^ ch) & 0xff]

    def __init__(self, pwd):
        self.key0 = 305419896
        self.key1 = 591751049
        self.key2 = 878082192
        for p in pwd:
            self._UpdateKeys(p)

    def _UpdateKeys(self, c):
        self.key0 = self._crc32(c, self.key0)
        self.key1 = (self.key1 + (self.key0 & 255)) & 4294967295
        self.key1 = (self.key1 * 134775813 + 1) & 4294967295
        self.key2 = self._crc32((self.key1 >> 24) & 255, self.key2)

    def __call__(self, c):
        """Decrypt a single character."""
        assert isinstance(c, int)
        k = self.key2 | 2
        c = c ^ (((k * (k^1)) >> 8) & 255)
        self._UpdateKeys(c)
        return c

class ZipExtFile:
    """File-like object for reading an archive member.
       Is returned by ZipFile.open().
    """

    def __init__(self, fileobj, zipinfo, decrypt=None):
        self.fileobj = fileobj
        self.decrypter = decrypt
        self.bytes_read = 0
        self.rawbuffer = b''
        self.readbuffer = b''
        self.linebuffer = b''
        self.eof = False
        self.univ_newlines = False
        self.nlSeps = (b"\n", )
        self.lastdiscard = b''

        self.compress_type = zipinfo.compress_type
        self.compress_size = zipinfo.compress_size

        self.closed  = False
        self.mode    = "r"
        self.name = zipinfo.filename

        # read from compressed files in 64k blocks
        self.compreadsize = 64*1024
        if self.compress_type == ZIP_DEFLATED:
            self.dc = zlib.decompressobj(-15)

    def set_univ_newlines(self, univ_newlines):
        self.univ_newlines = univ_newlines

        # pick line separator char(s) based on universal newlines flag
        self.nlSeps = (b"\n", )
        if self.univ_newlines:
            self.nlSeps = (b"\r\n", b"\r", b"\n")

    def __iter__(self):
        return self

    def __next__(self):
        nextline = self.readline()
        if not nextline:
            raise StopIteration()

        return nextline

    def close(self):
        self.closed = True

    def _checkfornewline(self):
        nl, nllen = -1, -1
        if self.linebuffer:
            # ugly check for cases where half of an \r\n pair was
            # read on the last pass, and the \r was discarded.  In this
            # case we just throw away the \n at the start of the buffer.
            if (self.lastdiscard, self.linebuffer[:1]) == (b'\r', b'\n'):
                self.linebuffer = self.linebuffer[1:]

            for sep in self.nlSeps:
                nl = self.linebuffer.find(sep)
                if nl >= 0:
                    nllen = len(sep)
                    return nl, nllen

        return nl, nllen

    def readline(self, size = -1):
        """Read a line with approx. size. If size is negative,
           read a whole line.
        """
        if size < 0:
            size = sys.maxsize
        elif size == 0:
            return b''

        # check for a newline already in buffer
        nl, nllen = self._checkfornewline()

        if nl >= 0:
            # the next line was already in the buffer
            nl = min(nl, size)
        else:
            # no line break in buffer - try to read more
            size -= len(self.linebuffer)
            while nl < 0 and size > 0:
                buf = self.read(min(size, 100))
                if not buf:
                    break
                self.linebuffer += buf
                size -= len(buf)

                # check for a newline in buffer
                nl, nllen = self._checkfornewline()

            # we either ran out of bytes in the file, or
            # met the specified size limit without finding a newline,
            # so return current buffer
            if nl < 0:
                s = self.linebuffer
                self.linebuffer = b''
                return s

        buf = self.linebuffer[:nl]
        self.lastdiscard = self.linebuffer[nl:nl + nllen]
        self.linebuffer = self.linebuffer[nl + nllen:]

        # line is always returned with \n as newline char (except possibly
        # for a final incomplete line in the file, which is handled above).
        return buf + b"\n"

    def readlines(self, sizehint = -1):
        """Return a list with all (following) lines. The sizehint parameter
        is ignored in this implementation.
        """
        result = []
        while True:
            line = self.readline()
            if not line: break
            result.append(line)
        return result

    def read(self, size = None):
        # act like file obj and return empty string if size is 0
        if size == 0:
            return b''

        # determine read size
        bytesToRead = self.compress_size - self.bytes_read

        # adjust read size for encrypted files since the first 12 bytes
        # are for the encryption/password information
        if self.decrypter is not None:
            bytesToRead -= 12

        if size is not None and size >= 0:
            if self.compress_type == ZIP_STORED:
                lr = len(self.readbuffer)
                bytesToRead = min(bytesToRead, size - lr)
            elif self.compress_type == ZIP_DEFLATED:
                if len(self.readbuffer) > size:
                    # the user has requested fewer bytes than we've already
                    # pulled through the decompressor; don't read any more
                    bytesToRead = 0
                else:
                    # user will use up the buffer, so read some more
                    lr = len(self.rawbuffer)
                    bytesToRead = min(bytesToRead, self.compreadsize - lr)

        # avoid reading past end of file contents
        if bytesToRead + self.bytes_read > self.compress_size:
            bytesToRead = self.compress_size - self.bytes_read

        # try to read from file (if necessary)
        if bytesToRead > 0:
            data = self.fileobj.read(bytesToRead)
            self.bytes_read += len(data)
            try:
                self.rawbuffer += data
            except:
                print(repr(self.fileobj), repr(self.rawbuffer),
                      repr(data))
                raise

            # handle contents of raw buffer
            if self.rawbuffer:
                newdata = self.rawbuffer
                self.rawbuffer = b''

                # decrypt new data if we were given an object to handle that
                if newdata and self.decrypter is not None:
                    newdata = bytes(map(self.decrypter, newdata))

                # decompress newly read data if necessary
                if newdata and self.compress_type == ZIP_DEFLATED:
                    newdata = self.dc.decompress(newdata)
                    self.rawbuffer = self.dc.unconsumed_tail
                    if self.eof and len(self.rawbuffer) == 0:
                        # we're out of raw bytes (both from the file and
                        # the local buffer); flush just to make sure the
                        # decompressor is done
                        newdata += self.dc.flush()
                        # prevent decompressor from being used again
                        self.dc = None

                self.readbuffer += newdata


        # return what the user asked for
        if size is None or len(self.readbuffer) <= size:
            data = self.readbuffer
            self.readbuffer = b''
        else:
            data = self.readbuffer[:size]
            self.readbuffer = self.readbuffer[size:]

        return data


class ZipFile:
    """ Class with methods to open, read, write, close, list zip files.

    z = ZipFile(file, mode="r", compression=ZIP_STORED, allowZip64=True)

    file: Either the path to the file, or a file-like object.
          If it is a path, the file will be opened and closed by ZipFile.
    mode: The mode can be either read "r", write "w" or append "a".
    compression: ZIP_STORED (no compression) or ZIP_DEFLATED (requires zlib).
    allowZip64: if True ZipFile will create files with ZIP64 extensions when
                needed, otherwise it will raise an exception when this would
                be necessary.

    """

    fp = None                   # Set here since __del__ checks it

    def __init__(self, file, mode="r", compression=ZIP_STORED, allowZip64=False):
        """Open the ZIP file with mode read "r", write "w" or append "a"."""
        if mode not in ("r", "w", "a"):
            raise RuntimeError('ZipFile() requires mode "r", "w", or "a"')

        if compression == ZIP_STORED:
            pass
        elif compression == ZIP_DEFLATED:
            if not zlib:
                raise RuntimeError(
                      "Compression requires the (missing) zlib module")
        else:
            raise RuntimeError("That compression method is not supported")

        self._allowZip64 = allowZip64
        self._didModify = False
        self.debug = 0  # Level of printing: 0 through 3
        self.NameToInfo = {}    # Find file info given name
        self.filelist = []      # List of ZipInfo instances for archive
        self.compression = compression  # Method of compression
        self.mode = key = mode.replace('b', '')[0]
        self.pwd = None

        # Check if we were passed a file-like object
        if isinstance(file, str):
            # No, it's a filename
            self._filePassed = 0
            self.filename = file
            modeDict = {'r' : 'rb', 'w': 'wb', 'a' : 'r+b'}
            try:
                self.fp = io.open(file, modeDict[mode])
            except IOError:
                if mode == 'a':
                    mode = key = 'w'
                    self.fp = io.open(file, modeDict[mode])
                else:
                    raise
        else:
            self._filePassed = 1
            self.fp = file
            self.filename = getattr(file, 'name', None)

        if key == 'r':
            self._GetContents()
        elif key == 'w':
            pass
        elif key == 'a':
            try:                        # See if file is a zip file
                self._RealGetContents()
                # seek to start of directory and overwrite
                self.fp.seek(self.start_dir, 0)
            except BadZipfile:          # file is not a zip file, just append
                self.fp.seek(0, 2)
        else:
            if not self._filePassed:
                self.fp.close()
                self.fp = None
            raise RuntimeError('Mode must be "r", "w" or "a"')

    def _GetContents(self):
        """Read the directory, making sure we close the file if the format
        is bad."""
        try:
            self._RealGetContents()
        except BadZipfile:
            if not self._filePassed:
                self.fp.close()
                self.fp = None
            raise

    def _RealGetContents(self):
        """Read in the table of contents for the ZIP file."""
        fp = self.fp
        endrec = _EndRecData(fp)
        if not endrec:
            raise BadZipfile("File is not a zip file")
        if self.debug > 1:
            print(endrec)
        size_cd = endrec[5]             # bytes in central directory
        offset_cd = endrec[6]   # offset of central directory
        self.comment = endrec[8]        # archive comment
        # endrec[9] is the offset of the "End of Central Dir" record
        if endrec[9] > ZIP64_LIMIT:
            x = endrec[9] - size_cd - 56 - 20
        else:
            x = endrec[9] - size_cd
        # "concat" is zero, unless zip was concatenated to another file
        concat = x - offset_cd
        if self.debug > 2:
            print("given, inferred, offset", offset_cd, x, concat)
        # self.start_dir:  Position of start of central directory
        self.start_dir = offset_cd + concat
        fp.seek(self.start_dir, 0)
        data = fp.read(size_cd)
        fp = io.BytesIO(data)
        total = 0
        while total < size_cd:
            centdir = fp.read(46)
            total = total + 46
            if centdir[0:4] != stringCentralDir:
                raise BadZipfile("Bad magic number for central directory")
            centdir = struct.unpack(structCentralDir, centdir)
            if self.debug > 2:
                print(centdir)
            filename = fp.read(centdir[_CD_FILENAME_LENGTH])
            # Create ZipInfo instance to store file information
            x = ZipInfo(filename.decode("utf-8"))
            x.extra = fp.read(centdir[_CD_EXTRA_FIELD_LENGTH])
            x.comment = fp.read(centdir[_CD_COMMENT_LENGTH])
            total = (total + centdir[_CD_FILENAME_LENGTH]
                     + centdir[_CD_EXTRA_FIELD_LENGTH]
                     + centdir[_CD_COMMENT_LENGTH])
            x.header_offset = centdir[_CD_LOCAL_HEADER_OFFSET]
            (x.create_version, x.create_system, x.extract_version, x.reserved,
                x.flag_bits, x.compress_type, t, d,
                x.CRC, x.compress_size, x.file_size) = centdir[1:12]
            x.volume, x.internal_attr, x.external_attr = centdir[15:18]
            # Convert date/time code to (year, month, day, hour, min, sec)
            x._raw_time = t
            x.date_time = ( (d>>9)+1980, (d>>5)&0xF, d&0x1F,
                                     t>>11, (t>>5)&0x3F, (t&0x1F) * 2 )

            x._decodeExtra()
            x.header_offset = x.header_offset + concat
            self.filelist.append(x)
            self.NameToInfo[x.filename] = x
            if self.debug > 2:
                print("total", total)


    def namelist(self):
        """Return a list of file names in the archive."""
        l = []
        for data in self.filelist:
            l.append(data.filename)
        return l

    def infolist(self):
        """Return a list of class ZipInfo instances for files in the
        archive."""
        return self.filelist

    def printdir(self, file=None):
        """Print a table of contents for the zip file."""
        print("%-46s %19s %12s" % ("File Name", "Modified    ", "Size"),
              file=file)
        for zinfo in self.filelist:
            date = "%d-%02d-%02d %02d:%02d:%02d" % zinfo.date_time[:6]
            print("%-46s %s %12d" % (zinfo.filename, date, zinfo.file_size),
                  file=file)

    def testzip(self):
        """Read all the files and check the CRC."""
        for zinfo in self.filelist:
            try:
                self.read(zinfo.filename)       # Check CRC-32
            except BadZipfile:
                return zinfo.filename


    def getinfo(self, name):
        """Return the instance of ZipInfo given 'name'."""
        info = self.NameToInfo.get(name)
        if info is None:
            raise KeyError(
                'There is no item named %r in the archive' % name)

        return info

    def setpassword(self, pwd):
        """Set default password for encrypted files."""
        assert isinstance(pwd, bytes)
        self.pwd = pwd

    def read(self, name, pwd=None):
        """Return file bytes (as a string) for name."""
        return self.open(name, "r", pwd).read()

    def open(self, name, mode="r", pwd=None):
        """Return file-like object for 'name'."""
        if mode not in ("r", "U", "rU"):
            raise RuntimeError('open() requires mode "r", "U", or "rU"')
        if not self.fp:
            raise RuntimeError(
                  "Attempt to read ZIP archive that was already closed")

        # Only open a new file for instances where we were not
        # given a file object in the constructor
        if self._filePassed:
            zef_file = self.fp
        else:
            zef_file = io.open(self.filename, 'rb')

        # Get info object for name
        zinfo = self.getinfo(name)

        filepos = zef_file.tell()

        zef_file.seek(zinfo.header_offset, 0)

        # Skip the file header:
        fheader = zef_file.read(30)
        if fheader[0:4] != stringFileHeader:
            raise BadZipfile("Bad magic number for file header")

        fheader = struct.unpack(structFileHeader, fheader)
        fname = zef_file.read(fheader[_FH_FILENAME_LENGTH])
        if fheader[_FH_EXTRA_FIELD_LENGTH]:
            zef_file.read(fheader[_FH_EXTRA_FIELD_LENGTH])

        if fname != zinfo.orig_filename.encode("utf-8"):
            raise BadZipfile(
                  'File name in directory %r and header %r differ.'
                  % (zinfo.orig_filename, fname))

        # check for encrypted flag & handle password
        is_encrypted = zinfo.flag_bits & 0x1
        zd = None
        if is_encrypted:
            if not pwd:
                pwd = self.pwd
            if not pwd:
                raise RuntimeError("File %s is encrypted, "
                                   "password required for extraction" % name)

            zd = _ZipDecrypter(pwd)
            # The first 12 bytes in the cypher stream is an encryption header
            #  used to strengthen the algorithm. The first 11 bytes are
            #  completely random, while the 12th contains the MSB of the CRC,
            #  or the MSB of the file time depending on the header type
            #  and is used to check the correctness of the password.
            bytes = zef_file.read(12)
            h = list(map(zd, bytes[0:12]))
            if zinfo.flag_bits & 0x8:
                # compare against the file type from extended local headers
                check_byte = (zinfo._raw_time >> 8) & 0xff
            else:
                # compare against the CRC otherwise
                check_byte = (zinfo.CRC >> 24) & 0xff
            if h[11] != check_byte:
                raise RuntimeError("Bad password for file", name)

        # build and return a ZipExtFile
        if zd is None:
            zef = ZipExtFile(zef_file, zinfo)
        else:
            zef = ZipExtFile(zef_file, zinfo, zd)

        # set universal newlines on ZipExtFile if necessary
        if "U" in mode:
            zef.set_univ_newlines(True)
        return zef

    def extract(self, member, path=None, pwd=None):
        """Extract a member from the archive to the current working directory,
           using its full name. Its file information is extracted as accurately
           as possible. `member' may be a filename or a ZipInfo object. You can
           specify a different directory using `path'.
        """
        if not isinstance(member, ZipInfo):
            member = self.getinfo(member)

        if path is None:
            path = os.getcwd()

        return self._extract_member(member, path, pwd)

    def extractall(self, path=None, members=None, pwd=None):
        """Extract all members from the archive to the current working
           directory. `path' specifies a different directory to extract to.
           `members' is optional and must be a subset of the list returned
           by namelist().
        """
        if members is None:
            members = self.namelist()

        for zipinfo in members:
            self.extract(zipinfo, path, pwd)

    def _extract_member(self, member, targetpath, pwd):
        """Extract the ZipInfo object 'member' to a physical
           file on the path targetpath.
        """
        # build the destination pathname, replacing
        # forward slashes to platform specific separators.
        if targetpath[-1:] == "/":
            targetpath = targetpath[:-1]

        # don't include leading "/" from file name if present
        if os.path.isabs(member.filename):
            targetpath = os.path.join(targetpath, member.filename[1:])
        else:
            targetpath = os.path.join(targetpath, member.filename)

        targetpath = os.path.normpath(targetpath)

        # Create all upper directories if necessary.
        upperdirs = os.path.dirname(targetpath)
        if upperdirs and not os.path.exists(upperdirs):
            os.makedirs(upperdirs)

        source = self.open(member.filename, pwd=pwd)
        target = open(targetpath, "wb")
        shutil.copyfileobj(source, target)
        source.close()
        target.close()

        return targetpath

    def _writecheck(self, zinfo):
        """Check for errors before writing a file to the archive."""
        if zinfo.filename in self.NameToInfo:
            if self.debug:      # Warning for duplicate names
                print("Duplicate name:", zinfo.filename)
        if self.mode not in ("w", "a"):
            raise RuntimeError('write() requires mode "w" or "a"')
        if not self.fp:
            raise RuntimeError(
                  "Attempt to write ZIP archive that was already closed")
        if zinfo.compress_type == ZIP_DEFLATED and not zlib:
            raise RuntimeError(
                  "Compression requires the (missing) zlib module")
        if zinfo.compress_type not in (ZIP_STORED, ZIP_DEFLATED):
            raise RuntimeError("That compression method is not supported")
        if zinfo.file_size > ZIP64_LIMIT:
            if not self._allowZip64:
                raise LargeZipFile("Filesize would require ZIP64 extensions")
        if zinfo.header_offset > ZIP64_LIMIT:
            if not self._allowZip64:
                raise LargeZipFile(
                      "Zipfile size would require ZIP64 extensions")

    def write(self, filename, arcname=None, compress_type=None):
        """Put the bytes from filename into the archive under the name
        arcname."""
        if not self.fp:
            raise RuntimeError(
                  "Attempt to write to ZIP archive that was already closed")

        st = os.stat(filename)
        mtime = time.localtime(st.st_mtime)
        date_time = mtime[0:6]
        # Create ZipInfo instance to store file information
        if arcname is None:
            arcname = filename
        arcname = os.path.normpath(os.path.splitdrive(arcname)[1])
        while arcname[0] in (os.sep, os.altsep):
            arcname = arcname[1:]
        zinfo = ZipInfo(arcname, date_time)
        zinfo.external_attr = (st[0] & 0xFFFF) << 16      # Unix attributes
        if compress_type is None:
            zinfo.compress_type = self.compression
        else:
            zinfo.compress_type = compress_type

        zinfo.file_size = st.st_size
        zinfo.flag_bits = 0x00
        zinfo.header_offset = self.fp.tell()    # Start of header bytes

        self._writecheck(zinfo)
        self._didModify = True
        fp = io.open(filename, "rb")
        # Must overwrite CRC and sizes with correct data later
        zinfo.CRC = CRC = 0
        zinfo.compress_size = compress_size = 0
        zinfo.file_size = file_size = 0
        self.fp.write(zinfo.FileHeader())
        if zinfo.compress_type == ZIP_DEFLATED:
            cmpr = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION,
                 zlib.DEFLATED, -15)
        else:
            cmpr = None
        while 1:
            buf = fp.read(1024 * 8)
            if not buf:
                break
            file_size = file_size + len(buf)
            CRC = crc32(buf, CRC) & 0xffffffff
            if cmpr:
                buf = cmpr.compress(buf)
                compress_size = compress_size + len(buf)
            self.fp.write(buf)
        fp.close()
        if cmpr:
            buf = cmpr.flush()
            compress_size = compress_size + len(buf)
            self.fp.write(buf)
            zinfo.compress_size = compress_size
        else:
            zinfo.compress_size = file_size
        zinfo.CRC = CRC
        zinfo.file_size = file_size
        # Seek backwards and write CRC and file sizes
        position = self.fp.tell()       # Preserve current position in file
        self.fp.seek(zinfo.header_offset + 14, 0)
        self.fp.write(struct.pack("<LLL", zinfo.CRC, zinfo.compress_size,
              zinfo.file_size))
        self.fp.seek(position, 0)
        self.filelist.append(zinfo)
        self.NameToInfo[zinfo.filename] = zinfo

    def writestr(self, zinfo_or_arcname, data):
        """Write a file into the archive.  The contents is 'data', which
        may be either a 'str' or a 'bytes' instance; if it is a 'str',
        it is encoded as UTF-8 first.
        'zinfo_or_arcname' is either a ZipInfo instance or
        the name of the file in the archive."""
        if isinstance(data, str):
            data = data.encode("utf-8")
        if not isinstance(zinfo_or_arcname, ZipInfo):
            zinfo = ZipInfo(filename=zinfo_or_arcname,
                            date_time=time.localtime(time.time())[:6])
            zinfo.compress_type = self.compression
        else:
            zinfo = zinfo_or_arcname

        if not self.fp:
            raise RuntimeError(
                  "Attempt to write to ZIP archive that was already closed")

        zinfo.file_size = len(data)            # Uncompressed size
        zinfo.header_offset = self.fp.tell()    # Start of header data
        self._writecheck(zinfo)
        self._didModify = True
        zinfo.CRC = crc32(data) & 0xffffffff       # CRC-32 checksum
        if zinfo.compress_type == ZIP_DEFLATED:
            co = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION,
                 zlib.DEFLATED, -15)
            data = co.compress(data) + co.flush()
            zinfo.compress_size = len(data)    # Compressed size
        else:
            zinfo.compress_size = zinfo.file_size
        zinfo.header_offset = self.fp.tell()    # Start of header data
        self.fp.write(zinfo.FileHeader())
        self.fp.write(data)
        self.fp.flush()
        if zinfo.flag_bits & 0x08:
            # Write CRC and file sizes after the file data
            self.fp.write(struct.pack("<lLL", zinfo.CRC, zinfo.compress_size,
                  zinfo.file_size))
        self.filelist.append(zinfo)
        self.NameToInfo[zinfo.filename] = zinfo

    def __del__(self):
        """Call the "close()" method in case the user forgot."""
        self.close()

    def close(self):
        """Close the file, and for mode "w" and "a" write the ending
        records."""
        if self.fp is None:
            return

        if self.mode in ("w", "a") and self._didModify: # write ending records
            count = 0
            pos1 = self.fp.tell()
            for zinfo in self.filelist:         # write central directory
                count = count + 1
                dt = zinfo.date_time
                dosdate = (dt[0] - 1980) << 9 | dt[1] << 5 | dt[2]
                dostime = dt[3] << 11 | dt[4] << 5 | (dt[5] // 2)
                extra = []
                if zinfo.file_size > ZIP64_LIMIT \
                        or zinfo.compress_size > ZIP64_LIMIT:
                    extra.append(zinfo.file_size)
                    extra.append(zinfo.compress_size)
                    file_size = 0xffffffff #-1
                    compress_size = 0xffffffff #-1
                else:
                    file_size = zinfo.file_size
                    compress_size = zinfo.compress_size

                if zinfo.header_offset > ZIP64_LIMIT:
                    extra.append(zinfo.header_offset)
                    header_offset = 0xffffffff  # -1 32 bit
                else:
                    header_offset = zinfo.header_offset

                extra_data = zinfo.extra
                if extra:
                    # Append a ZIP64 field to the extra's
                    extra_data = struct.pack(
                            '<HH' + 'Q'*len(extra),
                            1, 8*len(extra), *extra) + extra_data

                    extract_version = max(45, zinfo.extract_version)
                    create_version = max(45, zinfo.create_version)
                else:
                    extract_version = zinfo.extract_version
                    create_version = zinfo.create_version

                centdir = struct.pack(structCentralDir,
                 stringCentralDir, create_version,
                 zinfo.create_system, extract_version, zinfo.reserved,
                 zinfo.flag_bits, zinfo.compress_type, dostime, dosdate,
                 zinfo.CRC, compress_size, file_size,
                 len(zinfo.filename), len(extra_data), len(zinfo.comment),
                 0, zinfo.internal_attr, zinfo.external_attr,
                 header_offset)
                self.fp.write(centdir)
                self.fp.write(zinfo.filename.encode("utf-8"))
                self.fp.write(extra_data)
                self.fp.write(zinfo.comment)

            pos2 = self.fp.tell()
            # Write end-of-zip-archive record
            if pos1 > ZIP64_LIMIT:
                # Need to write the ZIP64 end-of-archive records
                zip64endrec = struct.pack(
                        structEndArchive64, stringEndArchive64,
                        44, 45, 45, 0, 0, count, count, pos2 - pos1, pos1)
                self.fp.write(zip64endrec)

                zip64locrec = struct.pack(
                        structEndArchive64Locator,
                        stringEndArchive64Locator, 0, pos2, 1)
                self.fp.write(zip64locrec)

                endrec = struct.pack(structEndArchive, stringEndArchive,
                            0, 0, count, count, pos2 - pos1, 0xffffffff, 0)
                self.fp.write(endrec)

            else:
                endrec = struct.pack(structEndArchive, stringEndArchive,
                         0, 0, count, count, pos2 - pos1, pos1, 0)
                self.fp.write(endrec)
            self.fp.flush()
        if not self._filePassed:
            self.fp.close()
        self.fp = None


class PyZipFile(ZipFile):
    """Class to create ZIP archives with Python library files and packages."""

    def writepy(self, pathname, basename = ""):
        """Add all files from "pathname" to the ZIP archive.

        If pathname is a package directory, search the directory and
        all package subdirectories recursively for all *.py and enter
        the modules into the archive.  If pathname is a plain
        directory, listdir *.py and enter all modules.  Else, pathname
        must be a Python *.py file and the module will be put into the
        archive.  Added modules are always module.pyo or module.pyc.
        This method will compile the module.py into module.pyc if
        necessary.
        """
        dir, name = os.path.split(pathname)
        if os.path.isdir(pathname):
            initname = os.path.join(pathname, "__init__.py")
            if os.path.isfile(initname):
                # This is a package directory, add it
                if basename:
                    basename = "%s/%s" % (basename, name)
                else:
                    basename = name
                if self.debug:
                    print("Adding package in", pathname, "as", basename)
                fname, arcname = self._get_codename(initname[0:-3], basename)
                if self.debug:
                    print("Adding", arcname)
                self.write(fname, arcname)
                dirlist = os.listdir(pathname)
                dirlist.remove("__init__.py")
                # Add all *.py files and package subdirectories
                for filename in dirlist:
                    path = os.path.join(pathname, filename)
                    root, ext = os.path.splitext(filename)
                    if os.path.isdir(path):
                        if os.path.isfile(os.path.join(path, "__init__.py")):
                            # This is a package directory, add it
                            self.writepy(path, basename)  # Recursive call
                    elif ext == ".py":
                        fname, arcname = self._get_codename(path[0:-3],
                                         basename)
                        if self.debug:
                            print("Adding", arcname)
                        self.write(fname, arcname)
            else:
                # This is NOT a package directory, add its files at top level
                if self.debug:
                    print("Adding files from directory", pathname)
                for filename in os.listdir(pathname):
                    path = os.path.join(pathname, filename)
                    root, ext = os.path.splitext(filename)
                    if ext == ".py":
                        fname, arcname = self._get_codename(path[0:-3],
                                         basename)
                        if self.debug:
                            print("Adding", arcname)
                        self.write(fname, arcname)
        else:
            if pathname[-3:] != ".py":
                raise RuntimeError(
                      'Files added with writepy() must end with ".py"')
            fname, arcname = self._get_codename(pathname[0:-3], basename)
            if self.debug:
                print("Adding file", arcname)
            self.write(fname, arcname)

    def _get_codename(self, pathname, basename):
        """Return (filename, archivename) for the path.

        Given a module name path, return the correct file path and
        archive name, compiling if necessary.  For example, given
        /python/lib/string, return (/python/lib/string.pyc, string).
        """
        file_py  = pathname + ".py"
        file_pyc = pathname + ".pyc"
        file_pyo = pathname + ".pyo"
        if os.path.isfile(file_pyo) and \
                            os.stat(file_pyo).st_mtime >= os.stat(file_py).st_mtime:
            fname = file_pyo    # Use .pyo file
        elif not os.path.isfile(file_pyc) or \
             os.stat(file_pyc).st_mtime < os.stat(file_py).st_mtime:
            import py_compile
            if self.debug:
                print("Compiling", file_py)
            try:
                py_compile.compile(file_py, file_pyc, None, True)
            except py_compile.PyCompileError as err:
                print(err.msg)
            fname = file_pyc
        else:
            fname = file_pyc
        archivename = os.path.split(fname)[1]
        if basename:
            archivename = "%s/%s" % (basename, archivename)
        return (fname, archivename)


def main(args = None):
    import textwrap
    USAGE=textwrap.dedent("""\
        Usage:
            zipfile.py -l zipfile.zip        # Show listing of a zipfile
            zipfile.py -t zipfile.zip        # Test if a zipfile is valid
            zipfile.py -e zipfile.zip target # Extract zipfile into target dir
            zipfile.py -c zipfile.zip src ... # Create zipfile from sources
        """)
    if args is None:
        args = sys.argv[1:]

    if not args or args[0] not in ('-l', '-c', '-e', '-t'):
        print(USAGE)
        sys.exit(1)

    if args[0] == '-l':
        if len(args) != 2:
            print(USAGE)
            sys.exit(1)
        zf = ZipFile(args[1], 'r')
        zf.printdir()
        zf.close()

    elif args[0] == '-t':
        if len(args) != 2:
            print(USAGE)
            sys.exit(1)
        zf = ZipFile(args[1], 'r')
        zf.testzip()
        print("Done testing")

    elif args[0] == '-e':
        if len(args) != 3:
            print(USAGE)
            sys.exit(1)

        zf = ZipFile(args[1], 'r')
        out = args[2]
        for path in zf.namelist():
            if path.startswith('./'):
                tgt = os.path.join(out, path[2:])
            else:
                tgt = os.path.join(out, path)

            tgtdir = os.path.dirname(tgt)
            if not os.path.exists(tgtdir):
                os.makedirs(tgtdir)
            fp = io.open(tgt, 'wb')
            fp.write(zf.read(path))
            fp.close()
        zf.close()

    elif args[0] == '-c':
        if len(args) < 3:
            print(USAGE)
            sys.exit(1)

        def addToZip(zf, path, zippath):
            if os.path.isfile(path):
                zf.write(path, zippath, ZIP_DEFLATED)
            elif os.path.isdir(path):
                for nm in os.listdir(path):
                    addToZip(zf,
                            os.path.join(path, nm), os.path.join(zippath, nm))
            # else: ignore

        zf = ZipFile(args[1], 'w', allowZip64=True)
        for src in args[2:]:
            addToZip(zf, src, os.path.basename(src))

        zf.close()

if __name__ == "__main__":
    main()
