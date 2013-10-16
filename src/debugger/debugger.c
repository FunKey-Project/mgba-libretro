#include "debugger.h"

#include "memory-debugger.h"

#include "arm.h"

#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct DebugVector {
	struct DebugVector* next;
	enum DVType {
		ERROR_TYPE,
		INT_TYPE,
		CHAR_TYPE
	} type;
	union {
		int32_t intValue;
		const char* charValue;
	};
};

static const char* ERROR_MISSING_ARGS = "Arguments missing";

static struct ARMDebugger* _activeDebugger;

typedef void (DebuggerComamnd)(struct ARMDebugger*, struct DebugVector*);

static void _breakInto(struct ARMDebugger*, struct DebugVector*);
static void _continue(struct ARMDebugger*, struct DebugVector*);
static void _next(struct ARMDebugger*, struct DebugVector*);
static void _print(struct ARMDebugger*, struct DebugVector*);
static void _printHex(struct ARMDebugger*, struct DebugVector*);
static void _printStatus(struct ARMDebugger*, struct DebugVector*);
static void _quit(struct ARMDebugger*, struct DebugVector*);
static void _readByte(struct ARMDebugger*, struct DebugVector*);
static void _readHalfword(struct ARMDebugger*, struct DebugVector*);
static void _readWord(struct ARMDebugger*, struct DebugVector*);
static void _setBreakpoint(struct ARMDebugger*, struct DebugVector*);
static void _setWatchpoint(struct ARMDebugger*, struct DebugVector*);

static void _breakIntoDefault(int signal);

static struct {
	const char* name;
	DebuggerComamnd* command;
} _debuggerCommands[] = {
	{ "b", _setBreakpoint },
	{ "break", _setBreakpoint },
	{ "c", _continue },
	{ "continue", _continue },
	{ "i", _printStatus },
	{ "info", _printStatus },
	{ "n", _next },
	{ "next", _next },
	{ "p", _print },
	{ "p/x", _printHex },
	{ "print", _print },
	{ "print/x", _printHex },
	{ "q", _quit },
	{ "quit", _quit },
	{ "rb", _readByte },
	{ "rh", _readHalfword },
	{ "rw", _readWord },
	{ "status", _printStatus },
	{ "w", _setWatchpoint },
	{ "watch", _setWatchpoint },
	{ "x", _breakInto },
	{ 0, 0 }
};

static inline void _printPSR(union PSR psr) {
	printf("%08X [%c%c%c%c%c%c%c]\n", psr.packed,
		psr.n ? 'N' : '-',
		psr.z ? 'Z' : '-',
		psr.c ? 'C' : '-',
		psr.v ? 'V' : '-',
		psr.i ? 'I' : '-',
		psr.f ? 'F' : '-',
		psr.t ? 'T' : '-');
}

static void _handleDeath(int sig) {
	(void)(sig);
	printf("No debugger attached!\n");
}

static void _breakInto(struct ARMDebugger* debugger, struct DebugVector* dv) {
	(void)(debugger);
	(void)(dv);
	sig_t oldSignal = signal(SIGTRAP, _handleDeath);
	kill(getpid(), SIGTRAP);
	signal(SIGTRAP, oldSignal);
}

static void _continue(struct ARMDebugger* debugger, struct DebugVector* dv) {
	(void)(dv);
	debugger->state = DEBUGGER_RUNNING;
}

static void _next(struct ARMDebugger* debugger, struct DebugVector* dv) {
	(void)(dv);
	ARMRun(debugger->cpu);
	_printStatus(debugger, 0);
}

static void _print(struct ARMDebugger* debugger, struct DebugVector* dv) {
	(void)(debugger);
	for ( ; dv; dv = dv->next) {
		printf(" %u", dv->intValue);
	}
	printf("\n");
}

static void _printHex(struct ARMDebugger* debugger, struct DebugVector* dv) {
	(void)(debugger);
	for ( ; dv; dv = dv->next) {
		printf(" 0x%08X", dv->intValue);
	}
	printf("\n");
}

static inline void _printLine(struct ARMDebugger* debugger, uint32_t address, enum ExecutionMode mode) {
	// TODO: write a disassembler
	if (mode == MODE_ARM) {
		uint32_t instruction = debugger->cpu->memory->load32(debugger->cpu->memory, address, 0);
		printf("%08X\n", instruction);
	} else {
		uint16_t instruction = debugger->cpu->memory->loadU16(debugger->cpu->memory, address, 0);
		printf("%04X\n", instruction);
	}
}

static void _printStatus(struct ARMDebugger* debugger, struct DebugVector* dv) {
	(void)(dv);
	int r;
	for (r = 0; r < 4; ++r) {
		printf("%08X %08X %08X %08X\n",
			debugger->cpu->gprs[r << 2],
			debugger->cpu->gprs[(r << 2) + 1],
			debugger->cpu->gprs[(r << 2) + 2],
			debugger->cpu->gprs[(r << 2) + 3]);
	}
	_printPSR(debugger->cpu->cpsr);
	int instructionLength;
	enum ExecutionMode mode = debugger->cpu->cpsr.t;
	if (mode == MODE_ARM) {
		instructionLength = WORD_SIZE_ARM;
	} else {
		instructionLength = WORD_SIZE_THUMB;
	}
	_printLine(debugger, debugger->cpu->gprs[ARM_PC] - instructionLength, mode);
}

static void _quit(struct ARMDebugger* debugger, struct DebugVector* dv) {
	(void)(dv);
	debugger->state = DEBUGGER_SHUTDOWN;
}

static void _readByte(struct ARMDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	uint8_t value = debugger->cpu->memory->loadU8(debugger->cpu->memory, address, 0);
	printf(" 0x%02X\n", value);
}

static void _readHalfword(struct ARMDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	uint16_t value = debugger->cpu->memory->loadU16(debugger->cpu->memory, address, 0);
	printf(" 0x%04X\n", value);
}

static void _readWord(struct ARMDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	uint32_t value = debugger->cpu->memory->load32(debugger->cpu->memory, address, 0);
	printf(" 0x%08X\n", value);
}

static void _setBreakpoint(struct ARMDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	struct DebugBreakpoint* breakpoint = malloc(sizeof(struct DebugBreakpoint));
	breakpoint->address = address;
	breakpoint->next = debugger->breakpoints;
	debugger->breakpoints = breakpoint;
}

static void _setWatchpoint(struct ARMDebugger* debugger, struct DebugVector* dv) {
	if (!dv || dv->type != INT_TYPE) {
		printf("%s\n", ERROR_MISSING_ARGS);
		return;
	}
	uint32_t address = dv->intValue;
	if (debugger->cpu->memory != &debugger->memoryShim.d) {
		ARMDebuggerInstallMemoryShim(debugger);
	}
	struct DebugBreakpoint* watchpoint = malloc(sizeof(struct DebugBreakpoint));
	watchpoint->address = address;
	watchpoint->next = debugger->memoryShim.watchpoints;
	debugger->memoryShim.watchpoints = watchpoint;
}

static void _checkBreakpoints(struct ARMDebugger* debugger) {
	struct DebugBreakpoint* breakpoint;
	int instructionLength;
	enum ExecutionMode mode = debugger->cpu->cpsr.t;
	if (mode == MODE_ARM) {
		instructionLength = WORD_SIZE_ARM;
	} else {
		instructionLength = WORD_SIZE_THUMB;
	}
	for (breakpoint = debugger->breakpoints; breakpoint; breakpoint = breakpoint->next) {
		if (breakpoint->address + instructionLength == debugger->cpu->gprs[ARM_PC]) {
			debugger->state = DEBUGGER_PAUSED;
			printf("Hit breakpoint\n");
			break;
		}
	}
}

static void _breakIntoDefault(int signal) {
	(void)(signal);
	_activeDebugger->state = DEBUGGER_PAUSED;
}

enum _DVParseState {
	PARSE_ERROR = -1,
	PARSE_ROOT = 0,
	PARSE_EXPECT_REGISTER,
	PARSE_EXPECT_REGISTER_2,
	PARSE_EXPECT_LR,
	PARSE_EXPECT_PC,
	PARSE_EXPECT_SP,
	PARSE_EXPECT_DECIMAL,
	PARSE_EXPECT_HEX,
	PARSE_EXPECT_PREFIX,
	PARSE_EXPECT_SUFFIX,
};

static struct DebugVector* _DVParse(struct ARMDebugger* debugger, const char* string, size_t length) {
	if (!string || length < 1) {
		return 0;
	}

	enum _DVParseState state = PARSE_ROOT;
	struct DebugVector dvTemp = { .type = INT_TYPE };
	uint32_t current = 0;

	while (length > 0 && string[0] && string[0] != ' ' && state != PARSE_ERROR) {
		char token = string[0];
		++string;
		--length;
		switch (state) {
		case PARSE_ROOT:
			switch (token) {
			case 'r':
				state = PARSE_EXPECT_REGISTER;
				break;
			case 'p':
				state = PARSE_EXPECT_PC;
				break;
			case 's':
				state = PARSE_EXPECT_SP;
				break;
			case 'l':
				state = PARSE_EXPECT_LR;
				break;
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				state = PARSE_EXPECT_DECIMAL;
				current = token - '0';
				break;
			case '0':
				state = PARSE_EXPECT_PREFIX;
				break;
			case '$':
				state = PARSE_EXPECT_HEX;
				current = 0;
				break;
			default:
				state = PARSE_ERROR;
				break;
			};
			break;
		case PARSE_EXPECT_LR:
			switch (token) {
			case 'r':
				current = debugger->cpu->gprs[ARM_LR];
				state = PARSE_EXPECT_SUFFIX;
				break;
			default:
				state = PARSE_ERROR;
				break;
			}
			break;
		case PARSE_EXPECT_PC:
			switch (token) {
			case 'c':
				current = debugger->cpu->gprs[ARM_PC];
				state = PARSE_EXPECT_SUFFIX;
				break;
			default:
				state = PARSE_ERROR;
				break;
			}
			break;
		case PARSE_EXPECT_SP:
			switch (token) {
			case 'p':
				current = debugger->cpu->gprs[ARM_SP];
				state = PARSE_EXPECT_SUFFIX;
				break;
			default:
				state = PARSE_ERROR;
				break;
			}
			break;
		case PARSE_EXPECT_REGISTER:
			switch (token) {
			case '0':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				current = debugger->cpu->gprs[token - '0'];
				state = PARSE_EXPECT_SUFFIX;
				break;
			case '1':
				state = PARSE_EXPECT_REGISTER_2;
				break;
			default:
				state = PARSE_ERROR;
				break;
			}
			break;
		case PARSE_EXPECT_REGISTER_2:
			switch (token) {
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
				current = debugger->cpu->gprs[token - '0' + 10];
				state = PARSE_EXPECT_SUFFIX;
				break;
			default:
				state = PARSE_ERROR;
				break;
			}
			break;
		case PARSE_EXPECT_DECIMAL:
			switch (token) {
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				// TODO: handle overflow
				current *= 10;
				current += token - '0';
				break;
			default:
				state = PARSE_ERROR;
			}
			break;
		case PARSE_EXPECT_HEX:
			switch (token) {
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				// TODO: handle overflow
				current *= 16;
				current += token - '0';
				break;
			case 'A':
			case 'B':
			case 'C':
			case 'D':
			case 'E':
			case 'F':
				// TODO: handle overflow
				current *= 16;
				current += token - 'A' + 10;
				break;
			case 'a':
			case 'b':
			case 'c':
			case 'd':
			case 'e':
			case 'f':
				// TODO: handle overflow
				current *= 16;
				current += token - 'a' + 10;
				break;
			default:
				state = PARSE_ERROR;
				break;
			}
			break;
		case PARSE_EXPECT_PREFIX:
			switch (token) {
			case 'X':
			case 'x':
				current = 0;
				state = PARSE_EXPECT_HEX;
				break;
			default:
				state = PARSE_ERROR;
				break;
			}
			break;
		case PARSE_EXPECT_SUFFIX:
			// TODO
			state = PARSE_ERROR;
			break;
		case PARSE_ERROR:
			// This shouldn't be reached
			break;
		}
	}

	struct DebugVector* dv = malloc(sizeof(struct DebugVector));
	if (state == PARSE_ERROR) {
		dv->type = ERROR_TYPE;
		dv->next = 0;
	} else {
		dvTemp.intValue = current;
		*dv = dvTemp;
		if (string[0] == ' ') {
			dv->next = _DVParse(debugger, string + 1, length - 1);
		}
	}
	return dv;
}

static void _DVFree(struct DebugVector* dv) {
	struct DebugVector* next;
	while (dv) {
		next = dv->next;
		free(dv);
		dv = next;
	}
}

static int _parse(struct ARMDebugger* debugger, const char* line, size_t count) {
	const char* firstSpace = strchr(line, ' ');
	size_t cmdLength;
	struct DebugVector* dv = 0;
	if (firstSpace) {
		cmdLength = firstSpace - line;
		dv = _DVParse(debugger, firstSpace + 1, count - cmdLength - 1);
		if (dv && dv->type == ERROR_TYPE) {
			printf("Parse error\n");
			_DVFree(dv);
			return 0;
		}
	} else {
		cmdLength = count;
	}

	int i;
	const char* name;
	for (i = 0; (name = _debuggerCommands[i].name); ++i) {
		if (strlen(name) != cmdLength) {
			continue;
		}
		if (strncasecmp(name, line, cmdLength) == 0) {
			_debuggerCommands[i].command(debugger, dv);
			_DVFree(dv);
			return 1;
		}
	}
	_DVFree(dv);
	printf("Command not found\n");
	return 0;
}

static char* _prompt(EditLine* el) {
	(void)(el);
	return "> ";
}

static void _commandLine(struct ARMDebugger* debugger) {
	const char* line;
	_printStatus(debugger, 0);
	int count = 0;
	HistEvent ev;
	while (debugger->state == DEBUGGER_PAUSED) {
		line = el_gets(debugger->elstate, &count);
		if (!line) {
			debugger->state = DEBUGGER_EXITING;
			return;
		}
		if (line[0] == '\n') {
			if (history(debugger->histate, &ev, H_FIRST) >= 0) {
				_parse(debugger, ev.str, strlen(ev.str) - 1);
			}
		} else {
			if (_parse(debugger, line, count - 1)) {
				history(debugger->histate, &ev, H_ENTER, line);
			}
		}
	}
}

static unsigned char _tabComplete(EditLine* elstate, int ch) {
	(void)(ch);
	const LineInfo* li = el_line(elstate);
	const char* commandPtr;
	int cmd = 0, len = 0;
	const char* name = 0;
	for (commandPtr = li->buffer; commandPtr <= li->cursor; ++commandPtr, ++len) {
		for (; (name = _debuggerCommands[cmd].name); ++cmd) {
			int cmp = strncasecmp(name, li->buffer, len);
			if (cmp > 0) {
				return CC_ERROR;
			}
			if (cmp == 0) {
				break;
			}
		}
	}
	if (_debuggerCommands[cmd + 1].name && strncasecmp(_debuggerCommands[cmd + 1].name, li->buffer, len - 1) == 0) {
		return CC_ERROR;
	}
	name += len - 1;
	el_insertstr(elstate, name);
	el_insertstr(elstate, " ");
	return CC_REDISPLAY;
}

void ARMDebuggerInit(struct ARMDebugger* debugger, struct ARMCore* cpu) {
	debugger->cpu = cpu;
	debugger->state = DEBUGGER_PAUSED;
	debugger->breakpoints = 0;
	// TODO: get argv[0]
	debugger->elstate = el_init("gbac", stdin, stdout, stderr);
	el_set(debugger->elstate, EL_PROMPT, _prompt);
	el_set(debugger->elstate, EL_EDITOR, "emacs");

	el_set(debugger->elstate, EL_CLIENTDATA, debugger);
	el_set(debugger->elstate, EL_ADDFN, "tab-complete", "Tab completion", _tabComplete);
	el_set(debugger->elstate, EL_BIND, "\t", "tab-complete", 0);
	debugger->histate = history_init();
	HistEvent ev;
	history(debugger->histate, &ev, H_SETSIZE, 200);
	el_set(debugger->elstate, EL_HIST, history, debugger->histate);
	debugger->memoryShim.p = debugger;
	debugger->memoryShim.watchpoints = 0;
	_activeDebugger = debugger;
	signal(SIGINT, _breakIntoDefault);
}

void ARMDebuggerDeinit(struct ARMDebugger* debugger) {
	// TODO: actually call this
	history_end(debugger->histate);
	el_end(debugger->elstate);
}

void ARMDebuggerRun(struct ARMDebugger* debugger) {
	if (debugger->state == DEBUGGER_EXITING) {
		debugger->state = DEBUGGER_RUNNING;
	}
	while (debugger->state < DEBUGGER_EXITING) {
		if (!debugger->breakpoints) {
			while (debugger->state == DEBUGGER_RUNNING) {
				ARMRun(debugger->cpu);
			}
		} else {
			while (debugger->state == DEBUGGER_RUNNING) {
				ARMRun(debugger->cpu);
				_checkBreakpoints(debugger);
			}
		}
		switch (debugger->state) {
		case DEBUGGER_RUNNING:
			break;
		case DEBUGGER_PAUSED:
			_commandLine(debugger);
			break;
		case DEBUGGER_EXITING:
		case DEBUGGER_SHUTDOWN:
			return;
		}
	}
}

void ARMDebuggerEnter(struct ARMDebugger* debugger) {
	debugger->state = DEBUGGER_PAUSED;
}
