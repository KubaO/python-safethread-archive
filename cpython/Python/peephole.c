/* Peephole optimizations for bytecode compiler. */

#include "Python.h"

#include "Python-ast.h"
#include "node.h"
#include "pyarena.h"
#include "ast.h"
#include "code.h"
#include "compile.h"
#include "symtable.h"
#include "opcode.h"

#define GETARG(arr, i) ((int)((arr[i+2]<<8) + arr[i+1]))
#define UNCONDITIONAL_JUMP(op)	(op==JUMP_ABSOLUTE || op==JUMP_FORWARD)
#define ABSOLUTE_JUMP(op) (op==JUMP_ABSOLUTE || op==CONTINUE_LOOP)
#define GETJUMPTGT(arr, i) (GETARG(arr,i) + (ABSOLUTE_JUMP(arr[i]) ? 0 : i+3))
#define SETARG(arr, i, val) arr[i+2] = val>>8; arr[i+1] = val & 255
#define CODESIZE(op)  (HAS_ARG(op) ? 3 : 1)
#define ISBASICBLOCK(blocks, start, bytes) \
	(blocks[start]==blocks[start+bytes-1])

/* Replace LOAD_CONST c1. LOAD_CONST c2 ... LOAD_CONST cn BUILD_TUPLE n
   with	   LOAD_CONST (c1, c2, ... cn).
   The consts table must still be in list form so that the
   new constant (c1, c2, ... cn) can be appended.
   Called with codestr pointing to the first LOAD_CONST.
   Bails out with no change if one or more of the LOAD_CONSTs is missing. 
   Also works for BUILD_LIST when followed by an "in" or "not in" test.
*/
static int
tuple_of_constants(unsigned char *codestr, Py_ssize_t n, PyObject *consts)
{
	PyObject *newconst, *constant;
	Py_ssize_t i, arg, len_consts;

	/* Pre-conditions */
	assert(PyList_CheckExact(consts));
	assert(codestr[n*3] == BUILD_TUPLE || codestr[n*3] == BUILD_LIST);
	assert(GETARG(codestr, (n*3)) == n);
	for (i=0 ; i<n ; i++)
		assert(codestr[i*3] == LOAD_CONST);

	/* Buildup new tuple of constants */
	newconst = PyTuple_New(n);
	if (newconst == NULL)
		return 0;
	len_consts = PyList_GET_SIZE(consts);
	for (i=0 ; i<n ; i++) {
		arg = GETARG(codestr, (i*3));
		assert(arg < len_consts);
		constant = PyList_GET_ITEM(consts, arg);
		Py_INCREF(constant);
		PyTuple_SET_ITEM(newconst, i, constant);
	}

	/* Append folded constant onto consts */
	if (PyList_Append(consts, newconst)) {
		Py_DECREF(newconst);
		return 0;
	}
	Py_DECREF(newconst);

	/* Write NOPs over old LOAD_CONSTS and
	   add a new LOAD_CONST newconst on top of the BUILD_TUPLE n */
	memset(codestr, NOP, n*3);
	codestr[n*3] = LOAD_CONST;
	SETARG(codestr, (n*3), len_consts);
	return 1;
}

/* Replace LOAD_CONST c1. LOAD_CONST c2 BINOP
   with	   LOAD_CONST binop(c1,c2)
   The consts table must still be in list form so that the
   new constant can be appended.
   Called with codestr pointing to the first LOAD_CONST. 
   Abandons the transformation if the folding fails (i.e.  1+'a').  
   If the new constant is a sequence, only folds when the size
   is below a threshold value.	That keeps pyc files from
   becoming large in the presence of code like:	 (None,)*1000.
*/
static int
fold_binops_on_constants(unsigned char *codestr, PyObject *consts)
{
	PyObject *newconst, *v, *w;
	Py_ssize_t len_consts, size;
	int opcode;

	/* Pre-conditions */
	assert(PyList_CheckExact(consts));
	assert(codestr[0] == LOAD_CONST);
	assert(codestr[3] == LOAD_CONST);

	/* Create new constant */
	v = PyList_GET_ITEM(consts, GETARG(codestr, 0));
	w = PyList_GET_ITEM(consts, GETARG(codestr, 3));
	opcode = codestr[6];
	switch (opcode) {
		case BINARY_POWER:
			newconst = PyNumber_Power(v, w, Py_None);
			break;
		case BINARY_MULTIPLY:
			newconst = PyNumber_Multiply(v, w);
			break;
		case BINARY_TRUE_DIVIDE:
			newconst = PyNumber_TrueDivide(v, w);
			break;
		case BINARY_FLOOR_DIVIDE:
			newconst = PyNumber_FloorDivide(v, w);
			break;
		case BINARY_MODULO:
			newconst = PyNumber_Remainder(v, w);
			break;
		case BINARY_ADD:
			newconst = PyNumber_Add(v, w);
			break;
		case BINARY_SUBTRACT:
			newconst = PyNumber_Subtract(v, w);
			break;
		case BINARY_SUBSCR:
			newconst = PyObject_GetItem(v, w);
			break;
		case BINARY_LSHIFT:
			newconst = PyNumber_Lshift(v, w);
			break;
		case BINARY_RSHIFT:
			newconst = PyNumber_Rshift(v, w);
			break;
		case BINARY_AND:
			newconst = PyNumber_And(v, w);
			break;
		case BINARY_XOR:
			newconst = PyNumber_Xor(v, w);
			break;
		case BINARY_OR:
			newconst = PyNumber_Or(v, w);
			break;
		default:
			/* Called with an unknown opcode */
			PyErr_Format(PyExc_SystemError,
			     "unexpected binary operation %d on a constant",
				     opcode);
			return 0;
	}
	if (newconst == NULL) {
		PyErr_Clear();
		return 0;
	}
	size = PyObject_Size(newconst);
	if (size == -1)
		PyErr_Clear();
	else if (size > 20) {
		Py_DECREF(newconst);
		return 0;
	}

	/* Append folded constant into consts table */
	len_consts = PyList_GET_SIZE(consts);
	if (PyList_Append(consts, newconst)) {
		Py_DECREF(newconst);
		return 0;
	}
	Py_DECREF(newconst);

	/* Write NOP NOP NOP NOP LOAD_CONST newconst */
	memset(codestr, NOP, 4);
	codestr[4] = LOAD_CONST;
	SETARG(codestr, 4, len_consts);
	return 1;
}

static int
fold_unaryops_on_constants(unsigned char *codestr, PyObject *consts)
{
	PyObject *newconst=NULL, *v;
	Py_ssize_t len_consts;
	int opcode;

	/* Pre-conditions */
	assert(PyList_CheckExact(consts));
	assert(codestr[0] == LOAD_CONST);

	/* Create new constant */
	v = PyList_GET_ITEM(consts, GETARG(codestr, 0));
	opcode = codestr[3];
	switch (opcode) {
		case UNARY_NEGATIVE:
			/* Preserve the sign of -0.0 */
			if (PyObject_IsTrue(v) == 1)
				newconst = PyNumber_Negative(v);
			break;
		case UNARY_INVERT:
			newconst = PyNumber_Invert(v);
			break;
		default:
			/* Called with an unknown opcode */
			PyErr_Format(PyExc_SystemError,
			     "unexpected unary operation %d on a constant",
				     opcode);
			return 0;
	}
	if (newconst == NULL) {
		PyErr_Clear();
		return 0;
	}

	/* Append folded constant into consts table */
	len_consts = PyList_GET_SIZE(consts);
	if (PyList_Append(consts, newconst)) {
		Py_DECREF(newconst);
		return 0;
	}
	Py_DECREF(newconst);

	/* Write NOP LOAD_CONST newconst */
	codestr[0] = NOP;
	codestr[1] = LOAD_CONST;
	SETARG(codestr, 1, len_consts);
	return 1;
}

static unsigned int *
markblocks(unsigned char *code, Py_ssize_t len)
{
	unsigned int *blocks = (unsigned int *)PyMem_Malloc(len*sizeof(int));
	int i,j, opcode, blockcnt = 0;

	if (blocks == NULL) {
		PyErr_NoMemory();
		return NULL;
	}
	memset(blocks, 0, len*sizeof(int));

	/* Mark labels in the first pass */
	for (i=0 ; i<len ; i+=CODESIZE(opcode)) {
		opcode = code[i];
		switch (opcode) {
			case FOR_ITER:
			case JUMP_FORWARD:
			case JUMP_IF_FALSE:
			case JUMP_IF_TRUE:
			case JUMP_ABSOLUTE:
			case CONTINUE_LOOP:
			case SETUP_LOOP:
			case SETUP_EXCEPT:
			case SETUP_FINALLY:
				j = GETJUMPTGT(code, i);
				blocks[j] = 1;
				break;
		}
	}
	/* Build block numbers in the second pass */
	for (i=0 ; i<len ; i++) {
		blockcnt += blocks[i];	/* increment blockcnt over labels */
		blocks[i] = blockcnt;
	}
	return blocks;
}

/* Helper to replace LOAD_NAME None/True/False with LOAD_CONST
   Returns: 0 if no change, 1 if change, -1 if error */
static int
load_global(unsigned char *codestr, Py_ssize_t i, char *name, PyObject *consts)
{
	Py_ssize_t j;
	PyObject *obj;
	if (name == NULL)
		return 0;
	if (strcmp(name, "None") == 0)
		obj = Py_None;
	else if (strcmp(name, "True") == 0)
		obj = Py_True;
	else if (strcmp(name, "False") == 0)
		obj = Py_False;
	else
		return 0;
	for (j = 0; j < PyList_GET_SIZE(consts); j++) {
		if (PyList_GET_ITEM(consts, j) == obj)
			break;
	}
	if (j == PyList_GET_SIZE(consts)) {
		if (PyList_Append(consts, obj) < 0)
			return -1;
	}
	assert(PyList_GET_ITEM(consts, j) == obj);
	codestr[i] = LOAD_CONST;
	SETARG(codestr, i, j);
	return 1;
}

/* Perform basic peephole optimizations to components of a code object.
   The consts object should still be in list form to allow new constants 
   to be appended.

   To keep the optimizer simple, it bails out (does nothing) for code that
   has a length over 32,700, and does not calculate extended arguments. 
   That allows us to avoid overflow and sign issues. Likewise, it bails when
   the lineno table has complex encoding for gaps >= 255. EXTENDED_ARG can
   appear before MAKE_FUNCTION; in this case both opcodes are skipped.
   EXTENDED_ARG preceding any other opcode causes the optimizer to bail.

   Optimizations are restricted to simple transformations occuring within a
   single basic block.	All transformations keep the code size the same or 
   smaller.  For those that reduce size, the gaps are initially filled with 
   NOPs.  Later those NOPs are removed and the jump addresses retargeted in 
   a single pass.  Line numbering is adjusted accordingly. */

PyObject *
PyCode_Optimize(PyObject *code, PyObject* consts, PyObject *names,
                PyObject *lineno_obj)
{
	Py_ssize_t i, j, codelen;
	int nops, h, adj;
	int tgt, tgttgt, opcode;
	unsigned char *codestr = NULL;
	unsigned char *lineno;
	int *addrmap = NULL;
	int new_line, cum_orig_line, last_line, tabsiz;
	int cumlc=0, lastlc=0;	/* Count runs of consecutive LOAD_CONSTs */
	unsigned int *blocks = NULL;
	char *name;

	/* Bail out if an exception is set */
	if (PyErr_Occurred())
		goto exitUnchanged;

	/* Bypass optimization when the lineno table is too complex */
	assert(PyString_Check(lineno_obj));
	lineno = (unsigned char*)PyString_AS_STRING(lineno_obj);
	tabsiz = PyString_GET_SIZE(lineno_obj);
	if (memchr(lineno, 255, tabsiz) != NULL)
		goto exitUnchanged;

	/* Avoid situations where jump retargeting could overflow */
	assert(PyString_Check(code));
	codelen = PyString_GET_SIZE(code);
	if (codelen > 32700)
		goto exitUnchanged;

	/* Make a modifiable copy of the code string */
	codestr = (unsigned char *)PyMem_Malloc(codelen);
	if (codestr == NULL)
		goto exitUnchanged;
	codestr = (unsigned char *)memcpy(codestr, 
					  PyString_AS_STRING(code), codelen);

	/* Verify that RETURN_VALUE terminates the codestring.	This allows
	   the various transformation patterns to look ahead several
	   instructions without additional checks to make sure they are not
	   looking beyond the end of the code string.
	*/
	if (codestr[codelen-1] != RETURN_VALUE)
		goto exitUnchanged;

	/* Mapping to new jump targets after NOPs are removed */
	addrmap = (int *)PyMem_Malloc(codelen * sizeof(int));
	if (addrmap == NULL)
		goto exitUnchanged;

	blocks = markblocks(codestr, codelen);
	if (blocks == NULL)
		goto exitUnchanged;
	assert(PyList_Check(consts));

	for (i=0 ; i<codelen ; i += CODESIZE(codestr[i])) {
		opcode = codestr[i];

		lastlc = cumlc;
		cumlc = 0;

		switch (opcode) {

			/* Replace UNARY_NOT JUMP_IF_FALSE POP_TOP with 
			   with	   JUMP_IF_TRUE POP_TOP */
			case UNARY_NOT:
				if (codestr[i+1] != JUMP_IF_FALSE  ||
				    codestr[i+4] != POP_TOP  ||
				    !ISBASICBLOCK(blocks,i,5))
					continue;
				tgt = GETJUMPTGT(codestr, (i+1));
				if (codestr[tgt] != POP_TOP)
					continue;
				j = GETARG(codestr, i+1) + 1;
				codestr[i] = JUMP_IF_TRUE;
				SETARG(codestr, i, j);
				codestr[i+3] = POP_TOP;
				codestr[i+4] = NOP;
				break;

				/* not a is b -->  a is not b
				   not a in b -->  a not in b
				   not a is not b -->  a is b
				   not a not in b -->  a in b
				*/
			case COMPARE_OP:
				j = GETARG(codestr, i);
				if (j < 6  ||  j > 9  ||
				    codestr[i+3] != UNARY_NOT  || 
				    !ISBASICBLOCK(blocks,i,4))
					continue;
				SETARG(codestr, i, (j^1));
				codestr[i+3] = NOP;
				break;

				/* Replace LOAD_GLOBAL/LOAD_NAME None/True/False
                                   with LOAD_CONST None/True/False */
			case LOAD_NAME:
			case LOAD_GLOBAL:
				j = GETARG(codestr, i);
				name = PyUnicode_AsString(PyTuple_GET_ITEM(names, j));
				h = load_global(codestr, i, name, consts);
				if (h < 0)
					goto exitUnchanged;
				else if (h == 0)
					continue;
				cumlc = lastlc + 1;
				break;

				/* Skip over LOAD_CONST trueconst
                                   JUMP_IF_FALSE xx  POP_TOP */
			case LOAD_CONST:
				cumlc = lastlc + 1;
				j = GETARG(codestr, i);
				if (codestr[i+3] != JUMP_IF_FALSE  ||
				    codestr[i+6] != POP_TOP  ||
				    !ISBASICBLOCK(blocks,i,7)  ||
				    !PyObject_IsTrue(PyList_GET_ITEM(consts, j)))
					continue;
				memset(codestr+i, NOP, 7);
				cumlc = 0;
				break;

				/* Try to fold tuples of constants (includes a case for lists
				   which are only used for "in" and "not in" tests).
				   Skip over BUILD_SEQN 1 UNPACK_SEQN 1.
				   Replace BUILD_SEQN 2 UNPACK_SEQN 2 with ROT2.
				   Replace BUILD_SEQN 3 UNPACK_SEQN 3 with ROT3 ROT2. */
			case BUILD_TUPLE:
			case BUILD_LIST:
				j = GETARG(codestr, i);
				h = i - 3 * j;
				if (h >= 0  &&
				    j <= lastlc	 &&
				    ((opcode == BUILD_TUPLE && 
				      ISBASICBLOCK(blocks, h, 3*(j+1))) ||
				     (opcode == BUILD_LIST && 
				      codestr[i+3]==COMPARE_OP && 
				      ISBASICBLOCK(blocks, h, 3*(j+2)) &&
				      (GETARG(codestr,i+3)==6 ||
				       GETARG(codestr,i+3)==7))) &&
				    tuple_of_constants(&codestr[h], j, consts)) {
					assert(codestr[i] == LOAD_CONST);
					cumlc = 1;
					break;
				}
				if (codestr[i+3] != UNPACK_SEQUENCE  ||
				    !ISBASICBLOCK(blocks,i,6) ||
				    j != GETARG(codestr, i+3))
					continue;
				if (j == 1) {
					memset(codestr+i, NOP, 6);
				} else if (j == 2) {
					codestr[i] = ROT_TWO;
					memset(codestr+i+1, NOP, 5);
				} else if (j == 3) {
					codestr[i] = ROT_THREE;
					codestr[i+1] = ROT_TWO;
					memset(codestr+i+2, NOP, 4);
				}
				break;

				/* Fold binary ops on constants.
				   LOAD_CONST c1 LOAD_CONST c2 BINOP -->  LOAD_CONST binop(c1,c2) */
			case BINARY_POWER:
			case BINARY_MULTIPLY:
			case BINARY_TRUE_DIVIDE:
			case BINARY_FLOOR_DIVIDE:
			case BINARY_MODULO:
			case BINARY_ADD:
			case BINARY_SUBTRACT:
			case BINARY_SUBSCR:
			case BINARY_LSHIFT:
			case BINARY_RSHIFT:
			case BINARY_AND:
			case BINARY_XOR:
			case BINARY_OR:
				if (lastlc >= 2	 &&
				    ISBASICBLOCK(blocks, i-6, 7)  &&
				    fold_binops_on_constants(&codestr[i-6], consts)) {
					i -= 2;
					assert(codestr[i] == LOAD_CONST);
					cumlc = 1;
				}
				break;

				/* Fold unary ops on constants.
				   LOAD_CONST c1  UNARY_OP -->	LOAD_CONST unary_op(c) */
			case UNARY_NEGATIVE:
			case UNARY_INVERT:
				if (lastlc >= 1	 &&
				    ISBASICBLOCK(blocks, i-3, 4)  &&
				    fold_unaryops_on_constants(&codestr[i-3], consts))	{
					i -= 2;
					assert(codestr[i] == LOAD_CONST);
					cumlc = 1;
				}
				break;

				/* Simplify conditional jump to conditional jump where the
				   result of the first test implies the success of a similar
				   test or the failure of the opposite test.
				   Arises in code like:
				   "if a and b:"
				   "if a or b:"
				   "a and b or c"
				   "(a and b) and c"
				   x:JUMP_IF_FALSE y   y:JUMP_IF_FALSE z  -->  x:JUMP_IF_FALSE z
				   x:JUMP_IF_FALSE y   y:JUMP_IF_TRUE z	 -->  x:JUMP_IF_FALSE y+3
				   where y+3 is the instruction following the second test.
				*/
			case JUMP_IF_FALSE:
			case JUMP_IF_TRUE:
				tgt = GETJUMPTGT(codestr, i);
				j = codestr[tgt];
				if (j == JUMP_IF_FALSE	||  j == JUMP_IF_TRUE) {
					if (j == opcode) {
						tgttgt = GETJUMPTGT(codestr, tgt) - i - 3;
						SETARG(codestr, i, tgttgt);
					} else {
						tgt -= i;
						SETARG(codestr, i, tgt);
					}
					break;
				}
				/* Intentional fallthrough */  

				/* Replace jumps to unconditional jumps */
			case FOR_ITER:
			case JUMP_FORWARD:
			case JUMP_ABSOLUTE:
			case CONTINUE_LOOP:
			case SETUP_LOOP:
			case SETUP_EXCEPT:
			case SETUP_FINALLY:
				tgt = GETJUMPTGT(codestr, i);
				/* Replace JUMP_* to a RETURN into just a RETURN */
				if (UNCONDITIONAL_JUMP(opcode) &&
				    codestr[tgt] == RETURN_VALUE) {
					codestr[i] = RETURN_VALUE;
					memset(codestr+i+1, NOP, 2);
					continue;
				}
				if (!UNCONDITIONAL_JUMP(codestr[tgt]))
					continue;
				tgttgt = GETJUMPTGT(codestr, tgt);
				if (opcode == JUMP_FORWARD) /* JMP_ABS can go backwards */
					opcode = JUMP_ABSOLUTE;
				if (!ABSOLUTE_JUMP(opcode))
					tgttgt -= i + 3;     /* Calc relative jump addr */
				if (tgttgt < 0)		  /* No backward relative jumps */
					continue;
				codestr[i] = opcode;
				SETARG(codestr, i, tgttgt);
				break;

			case EXTENDED_ARG:
				if (codestr[i+3] != MAKE_FUNCTION)
					goto exitUnchanged;
				/* don't visit MAKE_FUNCTION as GETARG will be wrong */
				i += 3;
				break;

				/* Replace RETURN LOAD_CONST None RETURN with just RETURN */
				/* Remove unreachable JUMPs after RETURN */
			case RETURN_VALUE:
				if (i+4 >= codelen)
					continue;
				if (codestr[i+4] == RETURN_VALUE &&
				    ISBASICBLOCK(blocks,i,5))
					memset(codestr+i+1, NOP, 4);
				else if (UNCONDITIONAL_JUMP(codestr[i+1]) &&
				         ISBASICBLOCK(blocks,i,4))
					memset(codestr+i+1, NOP, 3);
				break;
		}
	}

	/* Fixup linenotab */
	for (i=0, nops=0 ; i<codelen ; i += CODESIZE(codestr[i])) {
		addrmap[i] = i - nops;
		if (codestr[i] == NOP)
			nops++;
	}
	cum_orig_line = 0;
	last_line = 0;
	for (i=0 ; i < tabsiz ; i+=2) {
		cum_orig_line += lineno[i];
		new_line = addrmap[cum_orig_line];
		assert (new_line - last_line < 255);
		lineno[i] =((unsigned char)(new_line - last_line));
		last_line = new_line;
	}

	/* Remove NOPs and fixup jump targets */
	for (i=0, h=0 ; i<codelen ; ) {
		opcode = codestr[i];
		switch (opcode) {
			case NOP:
				i++;
				continue;

			case JUMP_ABSOLUTE:
			case CONTINUE_LOOP:
				j = addrmap[GETARG(codestr, i)];
				SETARG(codestr, i, j);
				break;

			case FOR_ITER:
			case JUMP_FORWARD:
			case JUMP_IF_FALSE:
			case JUMP_IF_TRUE:
			case SETUP_LOOP:
			case SETUP_EXCEPT:
			case SETUP_FINALLY:
				j = addrmap[GETARG(codestr, i) + i + 3] - addrmap[i] - 3;
				SETARG(codestr, i, j);
				break;
		}
		adj = CODESIZE(opcode);
		while (adj--)
			codestr[h++] = codestr[i++];
	}
	assert(h + nops == codelen);

	code = PyString_FromStringAndSize((char *)codestr, h);
	PyMem_Free(addrmap);
	PyMem_Free(codestr);
	PyMem_Free(blocks);
	return code;

 exitUnchanged:
	if (blocks != NULL)
		PyMem_Free(blocks);
	if (addrmap != NULL)
		PyMem_Free(addrmap);
	if (codestr != NULL)
		PyMem_Free(codestr);
	Py_INCREF(code);
	return code;
}
