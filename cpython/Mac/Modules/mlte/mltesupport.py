# This script generates a Python interface for an Apple Macintosh Manager.
# It uses the "bgen" package to generate C code.
# The function specifications are generated by scanning the mamager's header file,
# using the "scantools" package (customized for this particular manager).

#error missing SetActionFilter

import string

# Declarations that change for each manager
MODNAME = '_Mlte'                               # The name of the module

# The following is *usually* unchanged but may still require tuning
MODPREFIX = 'Mlte'                      # The prefix for module-wide routines
INPUTFILE = string.lower(MODPREFIX) + 'gen.py' # The file generated by the scanner
OUTPUTFILE = MODNAME + "module.c"       # The file generated by this program

from macsupport import *

# Create the type objects

includestuff = includestuff + """
#include <Carbon/Carbon.h>

/* For now we declare them forward here. They'll go to mactoolbox later */
static PyObject *TXNObj_New(TXNObject);
static int TXNObj_Convert(PyObject *, TXNObject *);
static PyObject *TXNFontMenuObj_New(TXNFontMenuObject);
static int TXNFontMenuObj_Convert(PyObject *, TXNFontMenuObject *);

// ADD declarations
#ifdef NOTYET_USE_TOOLBOX_OBJECT_GLUE
//extern PyObject *_CFTypeRefObj_New(CFTypeRef);
//extern int _CFTypeRefObj_Convert(PyObject *, CFTypeRef *);

//#define CFTypeRefObj_New _CFTypeRefObj_New
//#define CFTypeRefObj_Convert _CFTypeRefObj_Convert
#endif

/*
** Parse an optional fsspec
*/
static int
OptFSSpecPtr_Convert(PyObject *v, FSSpec **p_itself)
{
        static FSSpec fss;
        if (v == Py_None)
        {
                *p_itself = NULL;
                return 1;
        }
        *p_itself = &fss;
        return PyMac_GetFSSpec(v, *p_itself);
}

/*
** Parse an optional rect
*/
static int
OptRectPtr_Convert(PyObject *v, Rect **p_itself)
{
        static Rect r;

        if (v == Py_None)
        {
                *p_itself = NULL;
                return 1;
        }
        *p_itself = &r;
        return PyMac_GetRect(v, *p_itself);
}

/*
** Parse an optional GWorld
*/
static int
OptGWorldObj_Convert(PyObject *v, GWorldPtr *p_itself)
{
        if (v == Py_None)
        {
                *p_itself = NULL;
                return 1;
        }
        return GWorldObj_Convert(v, p_itself);
}

"""

initstuff = initstuff + """
//      PyMac_INIT_TOOLBOX_OBJECT_NEW(xxxx);
"""
TXNObject = OpaqueByValueType("TXNObject", "TXNObj")
TXNFontMenuObject = OpaqueByValueType("TXNFontMenuObject", "TXNFontMenuObj")

TXNFrameID = Type("TXNFrameID", "l")
TXNVersionValue = Type("TXNVersionValue", "l")
TXNFeatureBits = Type("TXNFeatureBits", "l")
TXNInitOptions = Type("TXNInitOptions", "l")
TXNFrameOptions = Type("TXNFrameOptions", "l")
TXNContinuousFlags = Type("TXNContinuousFlags", "l")
TXNMatchOptions = Type("TXNMatchOptions", "l")
TXNFileType = OSTypeType("TXNFileType")
TXNFrameType = Type("TXNFrameType", "l")
TXNDataType = OSTypeType("TXNDataType")
TXNControlTag = OSTypeType("TXNControlTag")
TXNActionKey = Type("TXNActionKey", "l")
TXNTabType = Type("TXNTabType", "b")
TXNScrollBarState = Type("TXNScrollBarState", "l")
TXNOffset = Type("TXNOffset", "l")
TXNObjectRefcon = FakeType("(TXNObjectRefcon)0") # XXXX For now...
TXNErrors = OSErrType("TXNErrors", "l")
TXNTypeRunAttributes = OSTypeType("TXNTypeRunAttributes")
TXNTypeRunAttributeSizes = Type("TXNTypeRunAttributeSizes", "l")
TXNPermanentTextEncodingType = Type("TXNPermanentTextEncodingType", "l")
TXTNTag = OSTypeType("TXTNTag")
TXNBackgroundType = Type("TXNBackgroundType", "l")
DragReference = OpaqueByValueType("DragReference", "DragObj")
DragTrackingMessage = Type("DragTrackingMessage", "h")
RgnHandle = OpaqueByValueType("RgnHandle", "ResObj")
OptRgnHandle = OpaqueByValueType("RgnHandle", "OptResObj")
GWorldPtr = OpaqueByValueType("GWorldPtr", "GWorldObj")
OptGWorldPtr = OpaqueByValueType("GWorldPtr", "OptGWorldObj")
MlteInBuffer = VarInputBufferType('void *', 'ByteCount', 'l')

OptFSSpecPtr = OpaqueByValueType("FSSpec *", "OptFSSpecPtr")
OptRectPtr = OpaqueByValueType("Rect *", "OptRectPtr")

UniChar = Type("UniChar", "h") # XXXX For now...
# ADD object type here

exec(open("mltetypetest.py").read())

# Our (opaque) objects

class TXNObjDefinition(PEP253Mixin, GlobalObjectDefinition):
    def outputCheckNewArg(self):
        Output("if (itself == NULL) return PyMac_Error(resNotFound);")

class TXNFontMenuObjDefinition(PEP253Mixin, GlobalObjectDefinition):
    def outputCheckNewArg(self):
        Output("if (itself == NULL) return PyMac_Error(resNotFound);")


# ADD object class here

# From here on it's basically all boiler plate...

# Create the generator groups and link them
module = MacModule(MODNAME, MODPREFIX, includestuff, finalstuff, initstuff)
TXNObject_object = TXNObjDefinition("TXNObject", "TXNObj", "TXNObject")
TXNFontMenuObject_object = TXNFontMenuObjDefinition("TXNFontMenuObject", "TXNFontMenuObj", "TXNFontMenuObject")

# ADD object here

module.addobject(TXNObject_object)
module.addobject(TXNFontMenuObject_object)
# ADD addobject call here

# Create the generator classes used to populate the lists
Function = OSErrWeakLinkFunctionGenerator
Method = OSErrWeakLinkMethodGenerator

# Create and populate the lists
functions = []
TXNObject_methods = []
TXNFontMenuObject_methods = []

# ADD _methods initializer here
exec(open(INPUTFILE).read())


# add the populated lists to the generator groups
# (in a different wordl the scan program would generate this)
for f in functions: module.add(f)
for f in TXNObject_methods: TXNObject_object.add(f)
for f in TXNFontMenuObject_methods: TXNFontMenuObject_object.add(f)

# ADD Manual generators here
inittextension_body = """
OSStatus _err;
TXNMacOSPreferredFontDescription * iDefaultFonts = NULL;
ItemCount iCountDefaultFonts = 0;
TXNInitOptions iUsageFlags;
PyMac_PRECHECK(TXNInitTextension);
if (!PyArg_ParseTuple(_args, "l", &iUsageFlags))
        return NULL;
_err = TXNInitTextension(iDefaultFonts,
                         iCountDefaultFonts,
                         iUsageFlags);
if (_err != noErr) return PyMac_Error(_err);
Py_INCREF(Py_None);
_res = Py_None;
return _res;
"""

f = ManualGenerator("TXNInitTextension", inittextension_body);
f.docstring = lambda: "(TXNInitOptions) -> None"
module.add(f)

# generate output (open the output file as late as possible)
SetOutputFileName(OUTPUTFILE)
module.generate()
