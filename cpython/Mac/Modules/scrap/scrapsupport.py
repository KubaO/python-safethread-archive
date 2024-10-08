# This script generates a Python interface for an Apple Macintosh Manager.
# It uses the "bgen" package to generate C code.
# The function specifications are generated by scanning the mamager's header file,
# using the "scantools" package (customized for this particular manager).

# NOTE: the scrap include file is so bad that the bgen output has to be
# massaged by hand.

import string

# Declarations that change for each manager
MACHEADERFILE = 'Scrap.h'               # The Apple header file
MODNAME = '_Scrap'                              # The name of the module
OBJECTNAME = 'Scrap'                    # The basic name of the objects used here

# The following is *usually* unchanged but may still require tuning
MODPREFIX = 'Scrap'                     # The prefix for module-wide routines
OBJECTTYPE = OBJECTNAME + 'Ref' # The C type used to represent them
OBJECTPREFIX = MODPREFIX + 'Obj'        # The prefix for object methods
INPUTFILE = string.lower(MODPREFIX) + 'gen.py' # The file generated by the scanner
OUTPUTFILE = '@' + MODNAME + "module.c" # The file generated by this program

from macsupport import *

# Create the type objects
ScrapRef = OpaqueByValueType(OBJECTTYPE, OBJECTPREFIX)

includestuff = includestuff + """
#include <Carbon/Carbon.h>

/*
** Generate ScrapInfo records
*/
static PyObject *
SCRRec_New(itself)
        ScrapStuff *itself;
{

        return Py_BuildValue("lO&hhO&", itself->scrapSize,
                ResObj_New, itself->scrapHandle, itself->scrapCount, itself->scrapState,
                PyMac_BuildStr255, itself->scrapName);
}
"""

ScrapStuffPtr = OpaqueByValueType('ScrapStuffPtr', 'SCRRec')
ScrapFlavorType = OSTypeType('ScrapFlavorType')
ScrapFlavorFlags = Type('ScrapFlavorFlags', 'l')
#ScrapFlavorInfo = OpaqueType('ScrapFlavorInfo', 'ScrapFlavorInfo')
putscrapbuffer = FixedInputBufferType('void *')

class MyObjectDefinition(PEP253Mixin, GlobalObjectDefinition):
    pass

# Create the generator groups and link them
module = MacModule(MODNAME, MODPREFIX, includestuff, finalstuff, initstuff)
object = MyObjectDefinition(OBJECTNAME, OBJECTPREFIX, OBJECTTYPE)
module.addobject(object)

# Create the generator classes used to populate the lists
Function = OSErrFunctionGenerator
Method = OSErrMethodGenerator

# Create and populate the lists
functions = []
methods = []
exec(open(INPUTFILE).read())

# add the populated lists to the generator groups
# (in a different wordl the scan program would generate this)
for f in functions: module.add(f)
for f in methods: object.add(f)

# generate output (open the output file as late as possible)
SetOutputFileName(OUTPUTFILE)
module.generate()
