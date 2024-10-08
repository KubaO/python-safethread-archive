
/* Return the initial module search path. */
/* Used by DOS, OS/2, Windows 3.1, Windows 95/98, Windows NT. */

/* ----------------------------------------------------------------
   PATH RULES FOR WINDOWS:
   This describes how sys.path is formed on Windows.  It describes the 
   functionality, not the implementation (ie, the order in which these 
   are actually fetched is different)

   * Python always adds an empty entry at the start, which corresponds
     to the current directory.

   * If the PYTHONPATH env. var. exists, its entries are added next.

   * We look in the registry for "application paths" - that is, sub-keys
     under the main PythonPath registry key.  These are added next (the
     order of sub-key processing is undefined).
     HKEY_CURRENT_USER is searched and added first.
     HKEY_LOCAL_MACHINE is searched and added next.
     (Note that all known installers only use HKLM, so HKCU is typically
     empty)

   * We attempt to locate the "Python Home" - if the PYTHONHOME env var
     is set, we believe it.  Otherwise, we use the path of our host .EXE's
     to try and locate our "landmark" (lib\\os.py) and deduce our home.
     - If we DO have a Python Home: The relevant sub-directories (Lib, 
       plat-win, lib-tk, etc) are based on the Python Home
     - If we DO NOT have a Python Home, the core Python Path is
       loaded from the registry.  This is the main PythonPath key, 
       and both HKLM and HKCU are combined to form the path)

   * Iff - we can not locate the Python Home, have not had a PYTHONPATH
     specified, and can't locate any Registry entries (ie, we have _nothing_
     we can assume is a good path), a default path with relative entries is 
     used (eg. .\Lib;.\plat-win, etc)


  The end result of all this is:
  * When running python.exe, or any other .exe in the main Python directory
    (either an installed version, or directly from the PCbuild directory),
    the core path is deduced, and the core paths in the registry are
    ignored.  Other "application paths" in the registry are always read.

  * When Python is hosted in another exe (different directory, embedded via 
    COM, etc), the Python Home will not be deduced, so the core path from
    the registry is used.  Other "application paths" in the registry are 
    always read.

  * If Python can't find its home and there is no registry (eg, frozen
    exe, some very strange installation setup) you get a path with
    some default, but relative, paths.

   ---------------------------------------------------------------- */


#include "Python.h"
#include "osdefs.h"
#include <wchar.h>

#ifdef MS_WINDOWS
#include <windows.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */

#include <string.h>

/* Search in some common locations for the associated Python libraries.
 *
 * Py_GetPath() tries to return a sensible Python module search path.
 *
 * The approach is an adaptation for Windows of the strategy used in
 * ../Modules/getpath.c; it uses the Windows Registry as one of its
 * information sources.
 */

#ifndef LANDMARK
#define LANDMARK L"lib\\os.py"
#endif

static wchar_t prefix[MAXPATHLEN+1];
static wchar_t progpath[MAXPATHLEN+1];
static wchar_t dllpath[MAXPATHLEN+1];
static wchar_t *module_search_path = NULL;


static int
is_sep(wchar_t ch)	/* determine if "ch" is a separator character */
{
#ifdef ALTSEP
	return ch == SEP || ch == ALTSEP;
#else
	return ch == SEP;
#endif
}

/* assumes 'dir' null terminated in bounds.  Never writes
   beyond existing terminator.
*/
static void
reduce(wchar_t *dir)
{
	size_t i = wcslen(dir);
	while (i > 0 && !is_sep(dir[i]))
		--i;
	dir[i] = '\0';
}
	

static int
exists(wchar_t *filename)
{
	struct _stat64 buf;
	return _wstat64(filename, &buf) == 0;
}

/* Assumes 'filename' MAXPATHLEN+1 bytes long - 
   may extend 'filename' by one character.
*/
static int
ismodule(wchar_t *filename)	/* Is module -- check for .pyc/.pyo too */
{
	if (exists(filename))
		return 1;

	/* Check for the compiled version of prefix. */
	if (wcslen(filename) < MAXPATHLEN) {
		wcscat(filename, Py_OptimizeFlag ? L"o" : L"c");
		if (exists(filename))
			return 1;
	}
	return 0;
}

/* Add a path component, by appending stuff to buffer.
   buffer must have at least MAXPATHLEN + 1 bytes allocated, and contain a
   NUL-terminated string with no more than MAXPATHLEN characters (not counting
   the trailing NUL).  It's a fatal error if it contains a string longer than
   that (callers must be careful!).  If these requirements are met, it's
   guaranteed that buffer will still be a NUL-terminated string with no more
   than MAXPATHLEN characters at exit.  If stuff is too long, only as much of
   stuff as fits will be appended.
*/
static void
join(wchar_t *buffer, wchar_t *stuff)
{
	size_t n, k;
	if (is_sep(stuff[0]))
		n = 0;
	else {
		n = wcslen(buffer);
		if (n > 0 && !is_sep(buffer[n-1]) && n < MAXPATHLEN)
			buffer[n++] = SEP;
	}
	if (n > MAXPATHLEN)
		Py_FatalError("buffer overflow in getpathp.c's joinpath()");
	k = wcslen(stuff);
	if (n + k > MAXPATHLEN)
		k = MAXPATHLEN - n;
	wcsncpy(buffer+n, stuff, k);
	buffer[n+k] = '\0';
}

/* gotlandmark only called by search_for_prefix, which ensures
   'prefix' is null terminated in bounds.  join() ensures
   'landmark' can not overflow prefix if too long.
*/
static int
gotlandmark(wchar_t *landmark)
{
	int ok;
	Py_ssize_t n;

	n = wcslen(prefix);
	join(prefix, landmark);
	ok = ismodule(prefix);
	prefix[n] = '\0';
	return ok;
}

/* assumes argv0_path is MAXPATHLEN+1 bytes long, already \0 term'd. 
   assumption provided by only caller, calculate_path() */
static int
search_for_prefix(wchar_t *argv0_path, wchar_t *landmark)
{
	/* Search from argv0_path, until landmark is found */
	wcscpy(prefix, argv0_path);
	do {
		if (gotlandmark(landmark))
			return 1;
		reduce(prefix);
	} while (prefix[0]);
	return 0;
}

#ifdef MS_WINDOWS

/* a string loaded from the DLL at startup.*/
extern const char *PyWin_DLLVersionString;


/* Load a PYTHONPATH value from the registry.
   Load from either HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER.

   Works in both Unicode and 8bit environments.  Only uses the
   Ex family of functions so it also works with Windows CE.

   Returns NULL, or a pointer that should be freed.

   XXX - this code is pretty strange, as it used to also
   work on Win16, where the buffer sizes werent available
   in advance.  It could be simplied now Win16/Win32s is dead!
*/

static wchar_t *
getpythonregpath(HKEY keyBase, int skipcore)
{
	HKEY newKey = 0;
	DWORD dataSize = 0;
	DWORD numKeys = 0;
	LONG rc;
	wchar_t *retval = NULL;
	WCHAR *dataBuf = NULL;
	static const WCHAR keyPrefix[] = L"Software\\Python\\PythonCore\\";
	static const WCHAR keySuffix[] = L"\\PythonPath";
	size_t versionLen;
	DWORD index;
	WCHAR *keyBuf = NULL;
	WCHAR *keyBufPtr;
	WCHAR **ppPaths = NULL;
	
	/* Tried to use sysget("winver") but here is too early :-( */
	versionLen = strlen(PyWin_DLLVersionString);
	/* Space for all the chars, plus one \0 */
	keyBuf = keyBufPtr = malloc(sizeof(keyPrefix) + 
		                    sizeof(WCHAR)*(versionLen-1) + 
				    sizeof(keySuffix));
	if (keyBuf==NULL) goto done;

	memcpy(keyBufPtr, keyPrefix, sizeof(keyPrefix)-sizeof(WCHAR));
	keyBufPtr += sizeof(keyPrefix)/sizeof(WCHAR) - 1;
	mbstowcs(keyBufPtr, PyWin_DLLVersionString, versionLen);
	keyBufPtr += versionLen;
	/* NULL comes with this one! */
	memcpy(keyBufPtr, keySuffix, sizeof(keySuffix));
	/* Open the root Python key */
	rc=RegOpenKeyExW(keyBase,
	                keyBuf, /* subkey */
	                0, /* reserved */
	                KEY_READ,
	                &newKey);
	if (rc!=ERROR_SUCCESS) goto done;
	/* Find out how big our core buffer is, and how many subkeys we have */
	rc = RegQueryInfoKey(newKey, NULL, NULL, NULL, &numKeys, NULL, NULL, 
	                NULL, NULL, &dataSize, NULL, NULL);
	if (rc!=ERROR_SUCCESS) goto done;
	if (skipcore) dataSize = 0; /* Only count core ones if we want them! */
	/* Allocate a temp array of char buffers, so we only need to loop 
	   reading the registry once
	*/
	ppPaths = malloc( sizeof(WCHAR *) * numKeys );
	if (ppPaths==NULL) goto done;
	memset(ppPaths, 0, sizeof(WCHAR *) * numKeys);
	/* Loop over all subkeys, allocating a temp sub-buffer. */
	for(index=0;index<numKeys;index++) {
		WCHAR keyBuf[MAX_PATH+1];
		HKEY subKey = 0;
		DWORD reqdSize = MAX_PATH+1;
		/* Get the sub-key name */
		DWORD rc = RegEnumKeyExW(newKey, index, keyBuf, &reqdSize,
					 NULL, NULL, NULL, NULL );
		if (rc!=ERROR_SUCCESS) goto done;
		/* Open the sub-key */
		rc=RegOpenKeyExW(newKey,
						keyBuf, /* subkey */
						0, /* reserved */
						KEY_READ,
						&subKey);
		if (rc!=ERROR_SUCCESS) goto done;
		/* Find the value of the buffer size, malloc, then read it */
		RegQueryValueExW(subKey, NULL, 0, NULL, NULL, &reqdSize);
		if (reqdSize) {
			ppPaths[index] = malloc(reqdSize);
			if (ppPaths[index]) {
				RegQueryValueExW(subKey, NULL, 0, NULL, 
				                (LPBYTE)ppPaths[index], 
				                &reqdSize);
				dataSize += reqdSize + 1; /* 1 for the ";" */
			}
		}
		RegCloseKey(subKey);
	}

	/* return null if no path to return */
	if (dataSize == 0) goto done;

	/* original datasize from RegQueryInfo doesn't include the \0 */
	dataBuf = malloc((dataSize+1) * sizeof(WCHAR));
	if (dataBuf) {
		WCHAR *szCur = dataBuf;
		DWORD reqdSize = dataSize;
		/* Copy our collected strings */
		for (index=0;index<numKeys;index++) {
			if (index > 0) {
				*(szCur++) = L';';
				dataSize--;
			}
			if (ppPaths[index]) {
				Py_ssize_t len = wcslen(ppPaths[index]);
				wcsncpy(szCur, ppPaths[index], len);
				szCur += len;
				assert(dataSize > (DWORD)len);
				dataSize -= (DWORD)len;
			}
		}
		if (skipcore)
			*szCur = '\0';
		else {
			/* If we have no values, we dont need a ';' */
			if (numKeys) {
				*(szCur++) = L';';
				dataSize--;
			}
			/* Now append the core path entries - 
			   this will include the NULL 
			*/
			rc = RegQueryValueExW(newKey, NULL, 0, NULL, 
					      (LPBYTE)szCur, &dataSize);
		}
		/* And set the result - caller must free */
		retval = dataBuf;
	}
done:
	/* Loop freeing my temp buffers */
	if (ppPaths) {
		for(index=0;index<numKeys;index++)
			if (ppPaths[index]) free(ppPaths[index]);
		free(ppPaths);
	}
	if (newKey)
		RegCloseKey(newKey);
	if (keyBuf)
		free(keyBuf);
	return retval;
}
#endif /* MS_WINDOWS */

static void
get_progpath(void)
{
	extern wchar_t *Py_GetProgramName(void);
	wchar_t *path = _wgetenv(L"PATH");
	wchar_t *prog = Py_GetProgramName();

#ifdef MS_WINDOWS
	extern HANDLE PyWin_DLLhModule;
	/* static init of progpath ensures final char remains \0 */
	if (PyWin_DLLhModule)
		if (!GetModuleFileNameW(PyWin_DLLhModule, dllpath, MAXPATHLEN))
			dllpath[0] = 0;
	if (GetModuleFileNameW(NULL, progpath, MAXPATHLEN))
		return;
#endif
	if (prog == NULL || *prog == '\0')
		prog = L"python";

	/* If there is no slash in the argv0 path, then we have to
	 * assume python is on the user's $PATH, since there's no
	 * other way to find a directory to start the search from.  If
	 * $PATH isn't exported, you lose.
	 */
#ifdef ALTSEP
	if (wcschr(prog, SEP) || wcschr(prog, ALTSEP))
#else
	if (wcschr(prog, SEP))
#endif
		wcsncpy(progpath, prog, MAXPATHLEN);
	else if (path) {
		while (1) {
			wchar_t *delim = wcschr(path, DELIM);

			if (delim) {
				size_t len = delim - path;
				/* ensure we can't overwrite buffer */
				len = min(MAXPATHLEN,len);
				wcsncpy(progpath, path, len);
				*(progpath + len) = '\0';
			}
			else
				wcsncpy(progpath, path, MAXPATHLEN);

			/* join() is safe for MAXPATHLEN+1 size buffer */
			join(progpath, prog);
			if (exists(progpath))
				break;

			if (!delim) {
				progpath[0] = '\0';
				break;
			}
			path = delim + 1;
		}
	}
	else
		progpath[0] = '\0';
}

static void
calculate_path(void)
{
	wchar_t argv0_path[MAXPATHLEN+1];
	wchar_t *buf;
	size_t bufsz;
	wchar_t *pythonhome = Py_GetPythonHome();
	char *_envpath = Py_GETENV("PYTHONPATH");
	wchar_t wenvpath[MAXPATHLEN+1];
	wchar_t *envpath = NULL;

#ifdef MS_WINDOWS
	int skiphome, skipdefault;
	wchar_t *machinepath = NULL;
	wchar_t *userpath = NULL;
	wchar_t zip_path[MAXPATHLEN+1];
	size_t len;
#endif
	if (_envpath) {
		size_t r = mbstowcs(wenvpath, _envpath, MAXPATHLEN+1);
		envpath = wenvpath;
		if (r == (size_t)-1 || r >= MAXPATHLEN)
			envpath = NULL;
	}

	get_progpath();
	/* progpath guaranteed \0 terminated in MAXPATH+1 bytes. */
	wcscpy(argv0_path, progpath);
	reduce(argv0_path);
	if (pythonhome == NULL || *pythonhome == '\0') {
		if (search_for_prefix(argv0_path, LANDMARK))
			pythonhome = prefix;
		else
			pythonhome = NULL;
	}
	else
		wcsncpy(prefix, pythonhome, MAXPATHLEN);

	if (envpath && *envpath == '\0')
		envpath = NULL;


#ifdef MS_WINDOWS
	/* Calculate zip archive path */
	if (dllpath[0])		/* use name of python DLL */
		wcsncpy(zip_path, dllpath, MAXPATHLEN);
	else			/* use name of executable program */
		wcsncpy(zip_path, progpath, MAXPATHLEN);
	zip_path[MAXPATHLEN] = '\0';
	len = wcslen(zip_path);
	if (len > 4) {
		zip_path[len-3] = 'z';	/* change ending to "zip" */
		zip_path[len-2] = 'i';
		zip_path[len-1] = 'p';
	}
	else {
		zip_path[0] = 0;
	}
 
	skiphome = pythonhome==NULL ? 0 : 1;
	machinepath = getpythonregpath(HKEY_LOCAL_MACHINE, skiphome);
	userpath = getpythonregpath(HKEY_CURRENT_USER, skiphome);
	/* We only use the default relative PYTHONPATH if we havent
	   anything better to use! */
	skipdefault = envpath!=NULL || pythonhome!=NULL || \
		      machinepath!=NULL || userpath!=NULL;
#endif

	/* We need to construct a path from the following parts.
	   (1) the PYTHONPATH environment variable, if set;
	   (2) for Win32, the zip archive file path;
	   (3) for Win32, the machinepath and userpath, if set;
	   (4) the PYTHONPATH config macro, with the leading "."
	       of each component replaced with pythonhome, if set;
	   (5) the directory containing the executable (argv0_path).
	   The length calculation calculates #4 first.
	   Extra rules:
	   - If PYTHONHOME is set (in any way) item (3) is ignored.
	   - If registry values are used, (4) and (5) are ignored.
	*/

	/* Calculate size of return buffer */
	if (pythonhome != NULL) {
		wchar_t *p;
		bufsz = 1;	
		for (p = PYTHONPATH; *p; p++) {
			if (*p == DELIM)
				bufsz++; /* number of DELIM plus one */
		}
		bufsz *= wcslen(pythonhome);
	}
	else
		bufsz = 0;
	bufsz += wcslen(PYTHONPATH) + 1;
	bufsz += wcslen(argv0_path) + 1;
#ifdef MS_WINDOWS
	if (userpath)
		bufsz += wcslen(userpath) + 1;
	if (machinepath)
		bufsz += wcslen(machinepath) + 1;
	bufsz += wcslen(zip_path) + 1;
#endif
	if (envpath != NULL)
		bufsz += wcslen(envpath) + 1;

	module_search_path = buf = malloc(bufsz*sizeof(wchar_t));
	if (buf == NULL) {
		/* We can't exit, so print a warning and limp along */
		fprintf(stderr, "Can't malloc dynamic PYTHONPATH.\n");
		if (envpath) {
			fprintf(stderr, "Using environment $PYTHONPATH.\n");
			module_search_path = envpath;
		}
		else {
			fprintf(stderr, "Using default static path.\n");
			module_search_path = PYTHONPATH;
		}
#ifdef MS_WINDOWS
		if (machinepath)
			free(machinepath);
		if (userpath)
			free(userpath);
#endif /* MS_WINDOWS */
		return;
	}

	if (envpath) {
		wcscpy(buf, envpath);
		buf = wcschr(buf, L'\0');
		*buf++ = DELIM;
	}
#ifdef MS_WINDOWS
	if (zip_path[0]) {
		wcscpy(buf, zip_path);
		buf = wcschr(buf, L'\0');
		*buf++ = DELIM;
	}
	if (userpath) {
		wcscpy(buf, userpath);
		buf = wcschr(buf, L'\0');
		*buf++ = DELIM;
		free(userpath);
	}
	if (machinepath) {
		wcscpy(buf, machinepath);
		buf = wcschr(buf, L'\0');
		*buf++ = DELIM;
		free(machinepath);
	}
	if (pythonhome == NULL) {
		if (!skipdefault) {
			wcscpy(buf, PYTHONPATH);
			buf = wcschr(buf, L'\0');
		}
	}
#else
	if (pythonhome == NULL) {
		wcscpy(buf, PYTHONPATH);
		buf = wcschr(buf, L'\0');
	}
#endif /* MS_WINDOWS */
	else {
		wchar_t *p = PYTHONPATH;
		wchar_t *q;
		size_t n;
		for (;;) {
			q = wcschr(p, DELIM);
			if (q == NULL)
				n = wcslen(p);
			else
				n = q-p;
			if (p[0] == '.' && is_sep(p[1])) {
				wcscpy(buf, pythonhome);
				buf = wcschr(buf, L'\0');
				p++;
				n--;
			}
			wcsncpy(buf, p, n);
			buf += n;
			if (q == NULL)
				break;
			*buf++ = DELIM;
			p = q+1;
		}
	}
	if (argv0_path) {
		*buf++ = DELIM;
		wcscpy(buf, argv0_path);
		buf = wcschr(buf, L'\0');
	}
	*buf = L'\0';
	/* Now to pull one last hack/trick.  If sys.prefix is
	   empty, then try and find it somewhere on the paths
	   we calculated.  We scan backwards, as our general policy
	   is that Python core directories are at the *end* of
	   sys.path.  We assume that our "lib" directory is
	   on the path, and that our 'prefix' directory is
	   the parent of that.
	*/
	if (*prefix==L'\0') {
		wchar_t lookBuf[MAXPATHLEN+1];
		wchar_t *look = buf - 1; /* 'buf' is at the end of the buffer */
		while (1) {
			Py_ssize_t nchars;
			wchar_t *lookEnd = look;
			/* 'look' will end up one character before the
			   start of the path in question - even if this
			   is one character before the start of the buffer
			*/
			while (look >= module_search_path && *look != DELIM)
				look--;
			nchars = lookEnd-look;
			wcsncpy(lookBuf, look+1, nchars);
			lookBuf[nchars] = L'\0';
			/* Up one level to the parent */
			reduce(lookBuf);
			if (search_for_prefix(lookBuf, LANDMARK)) {
				break;
			}
			/* If we are out of paths to search - give up */
			if (look < module_search_path)
				break;
			look--;
		}
	}
}


/* External interface */

wchar_t *
Py_GetPath(void)
{
	if (!module_search_path)
		calculate_path();
	return module_search_path;
}

wchar_t *
Py_GetPrefix(void)
{
	if (!module_search_path)
		calculate_path();
	return prefix;
}

wchar_t *
Py_GetExecPrefix(void)
{
	return Py_GetPrefix();
}

wchar_t *
Py_GetProgramFullPath(void)
{
	if (!module_search_path)
		calculate_path();
	return progpath;
}
