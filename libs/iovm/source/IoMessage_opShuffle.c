/*
 docCopyright("Jonathan Wright; Steve Dekorte (2002)", 2006)
 docLicense("BSD revised")
 */

#include "IoMessage_opShuffle.h"
#include "IoMap.h"
#include "IoNumber.h"

#define DATA(self) ((IoMessageData *)IoObject_dataPointer(self))

IoMap *IoState_createOperatorTable(IoState *state)
{
	typedef struct OpTable {
		char *symbol;
		int precedence;
	} OpTable;

	OpTable ops[] = {
		{"@",   0},
		{"@@",  0},
		{"'",   0},
		{".",   0},
		{"?",   0},
		{"(",   0},
		{")",   0},
			
		{"**",  1},
			
		{"++",  2},
		{"--",  2},
			
		{"*",   3},
		{"/",   3},
		{"%",   3},
			
		{"+",   4},
		{"-",   4},
			
		{"<<",  5},
		{">>",  5},
			
		{">",   6},
		{"<",   6},
		{"<=",  6},
		{">=",  6},
			
		{"==",  7},
		{"!=",  7},
			
		{"&",   8},
			
		{"^",   9},
			
		{"|",   10},
			
		{"and", 11},
		{"&&",  11},
			
		{"or",  12},
		{"||",  12},
			
		{"..",  13},
			
		{"+=",  14},
		{"-=",  14},
		{"*=",  14},
		{"/=",  14},
		{"%=",  14},
		{"&=",  14},
		{"^=",  14},
		{"|=",  14},
		{"<<=", 14},
		{">>=", 14},
		{"<-",  14},
		{"<->", 14},
		{"->",  14},
			
		{"return", 15},
			
		{",", 16},

		{NULL, 0},
	};
	
	IoMap *self = IoMap_new(state);
	OpTable *op = ops;

	while (op->symbol) 
	{
		IoMap_rawAtPut(self, IOSYMBOL(op->symbol), IONUMBER(op->precedence));
		op ++;
	}

	return self;
}

IoMap *IoState_createAssignOperatorTable(IoState *state)
{
	typedef struct {
		char *symbol;
		char *name;
	} OpTable;
	
	OpTable ops[] = {
		{":=", "setSlot"},
		{"=",  "updateSlot"},
		{"::=", "newSlot"},

		{NULL, 0},
	};
	
	IoMap *self = IoMap_new(state);
	OpTable *op = ops;

	while (op->symbol) 
	{
		IoMap_rawAtPut(self, IOSYMBOL(op->symbol), IOSYMBOL(op->name));
		op ++;
	}

	return self;
}


// TODO move the IO_OP_MAX_LEVEL define to be adjacent to the op table.
// TODO Set IO_OP_MAX_LEVEL to max-1 or something massive otherwise people playing with custom operators don't have many levels available
#define IO_OP_MAX_LEVEL 17

enum LevelType {ATTACH, ARG, NEW, UNUSED};

typedef struct {
	IoMessage *message;
	enum LevelType type;
	int precedence;
} Level;

void Level_finish(Level *self);
void Level_attachAndReplace(Level *self, IoMessage *msg);
void Level_setAwaitingFirstArg(Level *self, IoMessage *msg, int precedence);

typedef struct {
	Level pool[IO_OP_MAX_LEVEL];
	int currentLevel;
	
	List *stack;
	IoMap *operatorTable;
	IoMap *assignOperatorTable;
} Levels;

void Levels_reset(Levels *self)
{
	int i;
	self->currentLevel = 1;
	
	for (i = 0; i < IO_OP_MAX_LEVEL; i ++)
	{
		Level *level = &self->pool[i];
		level->type = UNUSED;
	}
	
	{
		Level *level = &self->pool[0];
		level->message = NULL;
		level->type = NEW;
		level->precedence = IO_OP_MAX_LEVEL;
	}
	
	List_removeAll(self->stack);
	List_append_(self->stack, &self->pool[0]);
}

// --- Levels ----------------------------------------------------------

Levels *Levels_new(IoMessage *msg)
{
	Levels *self = io_calloc(1, sizeof(Levels));

	IoState *state = IoObject_state(msg);
	IoSymbol *operatorTableSymbol = IoState_symbolWithCString_(state, "OperatorTable");

	// Lets be ultra flexable, and try to use the first message's operator table.
	IoObject *opTable = IoObject_rawGetSlot_(msg, operatorTableSymbol);

	// Otherwise, use Core OperatorTable, and if that doesn't exist, create it.
	if (opTable == NULL)
	{
		// There is a chance the message didn't have it, but the core did---due
		// to the Core not being part of the message's protos. Use Core
		// Message's OperatorTable
		opTable = IoObject_rawGetSlot_(state->core, operatorTableSymbol);
		
		// If Core doesn't have an OperatorTable, lets create it.
		if (opTable == NULL)
		{
			opTable = IoObject_new(state);
			IoObject_setSlot_to_(state->core, operatorTableSymbol, opTable);
		}
	}
	
	{
		IoSymbol *operatorsSymbol = IoState_symbolWithCString_(state, "operators");
		IoObject *operators = IoObject_rawGetSlot_(opTable, operatorsSymbol);
		
		if (operators && ISMAP(operators))
		{
			self->operatorTable = operators;
		}
		else
		{
			// Not strictly correct as if the message has it's own empty
			// OperatorTable slot, we'll create one for it instead of using
			// Core Message OperatorTable operators. Oh well.
			
			self->operatorTable = IoState_createOperatorTable(state);
			IoObject_setSlot_to_(opTable, operatorsSymbol, self->operatorTable);
		}
	}

	{
		IoSymbol *assignOperatorsSymbol = IoState_symbolWithCString_(state, "assignOperators");
		IoObject *assignOperators = IoObject_rawGetSlot_(opTable, assignOperatorsSymbol);
		
		if (assignOperators && ISMAP(assignOperators))
		{
			self->assignOperatorTable = assignOperators;
		}
		else
		{
			// Not strictly correct as if the message has it's own empty
			// OperatorTable slot, we'll create one for it instead of using
			// Core Message OperatorTable operators. Oh well.
			
			self->assignOperatorTable = IoState_createAssignOperatorTable(state);
			IoObject_setSlot_to_(opTable, assignOperatorsSymbol, self->assignOperatorTable);
		}
	}
	
	self->stack = List_new();
	Levels_reset(self);
	return self;
}

void Levels_free(Levels *self)
{
        List_free(self->stack);
	io_free(self);
}

inline Level *Levels_currentLevel(Levels *self)
{
	return List_top(self->stack);
}

void Levels_popDownTo(Levels *self, int targetLevel)
{
	Level *level;
	
	while (level = List_top(self->stack), level->precedence <= targetLevel && level->type != ARG)
	{
		Level_finish(List_pop(self->stack));
		self->currentLevel--;
	}
}

void Levels_attachToTopAndPush(Levels *self, IoMessage *msg, int precedence)
{
	Level *level = NULL;
	{
		Level *top = List_top(self->stack);
		Level_attachAndReplace(top, msg);
	}
	
	{
		// TODO: Check for overflow of the pool.
		if (self->currentLevel >= IO_OP_MAX_LEVEL)
		{
			IoState_error_(IoObject_state(msg), NULL, "compile error: Overflowed operator stack. Only %d levels of operators currently supported.", IO_OP_MAX_LEVEL-1);
		}
		
		level = &self->pool[self->currentLevel++];
		Level_setAwaitingFirstArg(level, msg, precedence);
		List_append_(self->stack, level);
	}
}

void Level_finish(Level *self)
{
	if (self->message)
	{
		IoMessage_rawSetNext(self->message, NULL);
	}
	
	self->type = UNUSED;
}

void Level_attach(Level *self, IoMessage *msg)
{
	switch (self->type)
	{
		case ATTACH:
			IoMessage_rawSetNext(self->message, msg);
			break;
			
		case ARG:
			IoMessage_addArg_(self->message, msg);
			break;
			
		case NEW:
			self->message = msg;
			break;
			
		case UNUSED:
			break;
	}
}

void Level_attachAndReplace(Level *self, IoMessage *msg)
{
	Level_attach(self, msg);
	self->type = ATTACH;
	self->message = msg;
}

void Level_setAwaitingFirstArg(Level *self, IoMessage *msg, int precedence)
{
	self->type = ARG;
	self->message = msg;
	self->precedence = precedence;
}

void Level_setAlreadyHasArgs(Level *self, IoMessage *msg)
{
	self->type = ATTACH;
	self->message = msg;
}

int Levels_levelForOp(Levels *self, char *messageName, IoSymbol *messageSymbol, IoMessage *msg)
{
	IoObject *value = IoMap_rawAt(self->operatorTable, messageSymbol);
	
	if (!value)
	{
		return -1;
	}
	
	if (ISNUMBER(value))
	{
		return IoNumber_asInt((IoNumber*)value);
	}
	else
	{
		IoState_error_(IoObject_state(msg), msg, "compile error: Value for '%s' in Message OperatorTable operators is not a number. Values in the OperatorTable operators are numbers which indicate the precedence of the operator.", messageName);
		return -1; // To keep the compiler happy.
	}
}

int Levels_isAssignOperator(Levels *self, IoSymbol *operator)
{
	return IoMap_rawAt(self->assignOperatorTable, operator) != NULL;
}

IoSymbol *Levels_nameForAssignOperator(Levels *self, IoState *state, IoSymbol *operator, IoSymbol *slotName, IoMessage *msg)
{
	IoObject *value = IoMap_rawAt(self->assignOperatorTable, operator);
	char *operatorString = CSTRING(operator);
	
	if (value != NULL && ISSYMBOL(value))
	{
		if (strcmp(operatorString, ":=") == 0 && isupper(CSTRING(slotName)[0]))
		{
			return state->setSlotWithTypeSymbol;
		}
		else
		{
			return value;
		}
	}
	else
	{
		IoState_error_(IoObject_state(msg), msg, "compile error: Value for '%s' in Message OperatorTable assignOperators is not a symbol. Values in the OperatorTable assignOperators are symbols which are the name of the operator.", operatorString);
		return NULL; // To keep the compiler happy.
	}
}

void Levels_attach(Levels *self, IoMessage *msg, List *expressions)
{
	// TODO clean up this method.
	
	IoSymbol *messageSymbol = IoMessage_name(msg);
	char *messageName = CSTRING(messageSymbol);
	int precedence = Levels_levelForOp(self, messageName, messageSymbol, msg);

	int msgArgCount = IoMessage_argCount(msg);
	
	// `o a := b c ; d`  becomes  `o setSlot("a", b c) ; d`
	// 
	// a      attaching
	// :=     msg
	// b c    msg->next
	
	if (Levels_isAssignOperator(self, messageSymbol))
	{
		IoState *state = IoObject_state(msg);
		Level *currentLevel = Levels_currentLevel(self);
		IoMessage *attaching = currentLevel->message;
		IoSymbol *setSlotName;
		
		if (attaching == NULL) // `:= b ;`
		{
			// Could be handled as, message(:= 42) -> setSlot(nil, 42)
			
			IoState_error_(state, msg, "compile error: %s requires a symbol to its left.", messageName);
		}
		
		if (IoMessage_argCount(attaching) > 0) // `a(1,2,3) := b ;`
		{
			IoState_error_(state, msg, "compile error: The symbol to the left of %s cannot have arguments.", messageName);
		}
		
		
		{
			// `a := b ;`
			IoSymbol *slotName = DATA(attaching)->name;
			IoSymbol *quotedSlotName = IoSeq_newSymbolWithFormat_(state, "\"%s\"", CSTRING(slotName));
			IoMessage *slotNameMessage = IoMessage_newWithName_returnsValue_(state, quotedSlotName, slotName);
			
			// `a := b ;`  ->  `a("a") := b ;`
			IoMessage_addArg_(attaching, slotNameMessage);

			setSlotName = Levels_nameForAssignOperator(self, state, messageSymbol, slotName, msg);
		}
		
		// `a("a") := b ;`  ->  `setSlot("a") := b ;`
		DATA(attaching)->name = IoObject_addingRef_(attaching, setSlotName);
		
		currentLevel->type = ATTACH;

		if (msgArgCount > 1) // `setSlot("a") :=(b, c, d) e ;`
		{
			IoState_error_(state, msg, "compile error: Assign operator passed multiple arguments, e.g., a := (b, c).", messageName);
		}
		
		if (msgArgCount > 0) // `setSlot("a") :=(b c) d e ;`
		{
			// `b c`
			IoMessage *arg = IoMessage_rawArgAt_(msg, 0);
			
                        if (DATA(msg)->next == NULL || IoMessage_rawIsEOL(DATA(msg)->next))
                        {
                                IoMessage_addArg_(attaching, arg);
                        }
                        else
                        {
                                // `()`
                                IoMessage *foo = IoMessage_newWithName_(state, IoState_symbolWithCString_(state, ""));
                                
                                // `()`  ->  `(b c)`
                                IoMessage_addArg_(foo, arg);
                                
                                // `(b c)`  ->  `(b c) d e ;`
                                IoMessage_rawSetNext(foo, DATA(msg)->next);
                                
                                // `setSlot("a") :=(b c) d e ;`  ->  `setSlot("a", (b c) d e ;) :=(b c) d e ;`
                                IoMessage_addArg_(attaching, foo);
                        }
		}
		else // `setSlot("a") := b ;`
		{
			// `setSlot("a") :=` or `setSlot("a") := ;`
			IoMessage *mn = DATA(msg)->next;
			IoSymbol *name = mn ? DATA(mn)->name : NULL;
			IoSymbol *semi = IoObject_state(msg)->semicolonSymbol;
			
			//if (mn == NULL || IoMessage_rawIsEOL(mn))
			if (mn == NULL || name == semi)
			{
				IoState_error_(state, msg, "compile error: %s must be followed by a value.", messageName);
			}
			
			// `setSlot("a") := b c ;`  ->  `setSlot("a", b c ;) := b c ;`
			IoMessage_addArg_(attaching, DATA(msg)->next);
		}
		
		// process the value (`b c d`) later  (`setSlot("a", b c d) := b c d ;`)
		if (DATA(msg)->next != NULL && !IoMessage_rawIsEOL(DATA(msg)->next))
		{
			List_push_(expressions, DATA(msg)->next);
		}
		
		{
			IoMessage *last = msg;
			while (DATA(last)->next != NULL && !IoMessage_rawIsEOL(DATA(last)->next))
			{
				last = DATA(last)->next;
			}

			IoMessage_rawSetNext(attaching, DATA(last)->next);
			
			// Continue processing in IoMessage_opShuffle loop
			IoMessage_rawSetNext(msg, DATA(last)->next);
			
                        if (last != msg)
                        {
                                IoMessage_rawSetNext(last, NULL);
                        }
		}
		
		// make sure b in `1 := b` gets executed
		IoMessage_cachedResult_(attaching, NULL);
	}
	else if (IoMessage_rawIsEOL(msg))
	{
		Levels_popDownTo(self, IO_OP_MAX_LEVEL-1);
		Level_attachAndReplace(Levels_currentLevel(self), msg);
	}
	else if (precedence != -1)
	{
		if (msgArgCount > 0)
		{
			Level_setAlreadyHasArgs(Levels_currentLevel(self), msg);
		}
		else
		{
			Levels_popDownTo(self, precedence);
			Levels_attachToTopAndPush(self, msg, precedence);
		}
	}
	else
	{
		Level_attachAndReplace(Levels_currentLevel(self), msg);
	}
}

void Levels_nextMessage(Levels *self)
{
	Level *level;
	
	while ((level = List_pop(self->stack)))
	{
		Level_finish(level);
	}
	
	Levels_reset(self);
}

void IoMessage_opShuffle_(IoMessage *self)
{
	if (IoObject_rawGetSlot_(self, IOSTATE->opShuffleSymbol) && IoMessage_name(self) != IOSTATE->noShufflingSymbol)
	{
		IoMessage_locals_performOn_(IOSTATE->opShuffleMessage, IOSTATE->lobby, self);
	}
}

IoMessage *IoMessage_opShuffle(IoMessage *self, IoObject *locals, IoMessage *m)
{
	Levels *levels = Levels_new(self);
	List *expressions = List_new();
	
	List_push_(expressions, self);
	
	while (List_size(expressions) >= 1)
	{
		IoMessage *n = List_pop(expressions);
		
		do
		{
			Levels_attach(levels, n, expressions);
			List_appendSeq_(expressions, DATA(n)->args);
		} while (n = DATA(n)->next);
		
		Levels_nextMessage(levels);
	}

    List_free(expressions);
	Levels_free(levels);

	return self;
}
