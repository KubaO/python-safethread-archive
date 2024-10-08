# This script generates a Python interface for an Apple Macintosh Manager.
# It uses the "bgen" package to generate C code.
# The function specifications are generated by scanning the mamager's header file,
# using the "scantools" package (customized for this particular manager).

import string

# Declarations that change for each manager
MACHEADERFILE = 'Fonts.h'               # The Apple header file
MODNAME = '_Fm'                         # The name of the module

# The following is *usually* unchanged but may still require tuning
MODPREFIX = 'Fm'                        # The prefix for module-wide routines
INPUTFILE = string.lower(MODPREFIX) + 'gen.py' # The file generated by the scanner
OUTPUTFILE = MODNAME + "module.c"       # The file generated by this program

from macsupport import *

# Create the type objects

class RevVarInputBufferType(VarInputBufferType):
    def passInput(self, name):
        return "%s__len__, %s__in__" % (name, name)

TextBuffer = RevVarInputBufferType()


includestuff = includestuff + """
#include <Carbon/Carbon.h>


/*
** Parse/generate ComponentDescriptor records
*/
static PyObject *
FMRec_New(FMetricRec *itself)
{

        return Py_BuildValue("O&O&O&O&O&",
                PyMac_BuildFixed, itself->ascent,
                PyMac_BuildFixed, itself->descent,
                PyMac_BuildFixed, itself->leading,
                PyMac_BuildFixed, itself->widMax,
                ResObj_New, itself->wTabHandle);
}

#if 0
/* Not needed... */
static int
FMRec_Convert(PyObject *v, FMetricRec *p_itself)
{
        return PyArg_ParseTuple(v, "O&O&O&O&O&",
                PyMac_GetFixed, &itself->ascent,
                PyMac_GetFixed, &itself->descent,
                PyMac_GetFixed, &itself->leading,
                PyMac_GetFixed, &itself->widMax,
                ResObj_Convert, &itself->wTabHandle);
}
#endif

"""

FMetricRecPtr = OpaqueType('FMetricRec', 'FMRec')

# Create the generator groups and link them
module = MacModule(MODNAME, MODPREFIX, includestuff, finalstuff, initstuff)

# Create the generator classes used to populate the lists
Function = OSErrWeakLinkFunctionGenerator

# Create and populate the lists
functions = []
exec(open(INPUTFILE).read())

# add the populated lists to the generator groups
# (in a different wordl the scan program would generate this)
for f in functions: module.add(f)

# generate output (open the output file as late as possible)
SetOutputFileName(OUTPUTFILE)
module.generate()
