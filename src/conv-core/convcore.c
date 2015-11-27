/** 
  @defgroup Converse
  \brief Converse--a parallel portability layer.

  Converse is the lowest level inside the Charm++ hierarchy. It stands on top
  of the machine layer, and it provides all the common functionality across
  platforms.

  One converse program is running on every processor (or node in the smp
  version). it manages the message transmission, and the memory allocation.
  Charm++, which is on top of Converse, uses its functionality for
  interprocess *communication.

  In order to maintain multiple independent objects inside a single user space
  program, it uses a personalized version of threads, which can be executed,
  suspended, and migrated across processors.

  It provides a scheduler for message delivery: methods can be registered to
  the scheduler, and then messages allocated through CmiAlloc can be sent to
  the correspondent method in a remote processor. This is done through the
  converse header (which has few common fields, but is architecture dependent).

  @defgroup ConverseScheduler 
  \brief The portion of Converse responsible for scheduling the execution of 
  incoming messages.
  
  Converse provides a scheduler for message delivery: methods can be registered to 
  the scheduler, and then messages allocated through CmiAlloc can be sent to 
  the correspondent method in a remote processor. This is done through the
  converse header (which has few common fields, but is architecture dependent).

  In converse the CsdScheduleForever() routine will run an infinite while loop that
  looks for available messages to process from the unprocessed message queues. The
  details of the queues and the order in which they are emptied is hidden behind
  CsdNextMessage(), which is used to dequeue the next message for processing by the
  converse scheduler. When a message is taken from the queue it is then passed into
  CmiHandleMessage() which calls the handler associated with the message.

  Incoming messages that are destined for Charm++ will be passed to the 
  \ref CharmScheduler "charm scheduling routines".

  @file
  converse main core
  @ingroup Converse
  @ingroup ConverseScheduler
 
  @addtogroup Converse
  @{

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/time.h>
#include <sys/resource.h>
#else
#define snprintf _snprintf
#endif

#include "converse.h"
#include "conv-trace.h"
#include "sockRoutines.h"
#include "queueing.h"
#include "conv-ccs.h"
#include "ccs-server.h"
#include "memory-isomalloc.h"
#if CMK_PROJECTOR
#include "converseEvents.h"             /* projector */
#include "traceCoreCommon.h"    /* projector */
#include "machineEvents.h"     /* projector */
#endif

extern const char * const CmiCommitID;

#if CMK_BIGSIM_CHARM
extern void initQd(char **argv);
#endif

#if CONVERSE_POOL
#include "cmipool.h"
#endif

#if CMK_CONDS_USE_SPECIAL_CODE
CmiSwitchToPEFnPtr CmiSwitchToPE;
#endif

CpvExtern(int, _traceCoreOn);   /* projector */
extern void CcdModuleInit(char **);
extern void CmiMemoryInit(char **);
extern void CldModuleInit(char **);

#if CMK_WHEN_PROCESSOR_IDLE_USLEEP
#include <sys/types.h>
#include <sys/time.h>
#endif

#if CMK_TIMER_USE_TIMES
#include <sys/times.h>
#include <limits.h>
#include <unistd.h>
#endif

#if CMK_TIMER_USE_GETRUSAGE
#include <sys/time.h>
#include <sys/resource.h>
#endif

#if CMK_TIMER_USE_RDTSC
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifdef CMK_TIMER_USE_WIN32API
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/timeb.h>
#endif

#ifdef CMK_HAS_ASCTIME
#include <time.h>
#endif

#ifdef CMK_CUDA
#include "cuda-hybrid-api.h"
#endif

#include "quiescence.h"

#if USE_MPI_CTRLMSG_SCHEME && CMK_CONVERSE_MPI
#include <mpi.h>
#endif

//int cur_restart_phase = 1;      /* checkpointing/restarting phase counter */
CpvDeclare(int,_curRestartPhase);
static int CsdLocalMax = CSD_LOCAL_MAX_DEFAULT;

int CharmLibInterOperate = 0;
CpvDeclare(int,interopExitFlag);

CpvStaticDeclare(int, CmiMainHandlerIDP); /* Main handler for _CmiMultipleSend that is run on every node */

#if CMK_MEM_CHECKPOINT
void (*notify_crash_fn)(int) = NULL;
#endif

CpvDeclare(char *, _validProcessors);

/*****************************************************************************
 *
 * Unix Stub Functions
 *
 ****************************************************************************/

#ifdef MEMMONITOR
typedef unsigned long mmulong;
CpvDeclare(mmulong,MemoryUsage);
CpvDeclare(mmulong,HiWaterMark);
CpvDeclare(mmulong,ReportedHiWaterMark);
CpvDeclare(int,AllocCount);
CpvDeclare(int,BlocksAllocated);
#endif

#define MAX_HANDLERS 512

#if ! CMK_CMIPRINTF_IS_A_BUILTIN
CpvDeclare(int,expIOFlushFlag);
#if CMI_IO_BUFFER_EXPLICIT
/* 250k not too large depending on how slow terminal IO is */
#define DEFAULT_IO_BUFFER_SIZE 250000
CpvDeclare(char*,explicitIOBuffer);
CpvDeclare(int,expIOBufferSize);
#endif
#endif

#if CMK_NODE_QUEUE_AVAILABLE
void  *CmiGetNonLocalNodeQ();
#endif

CpvDeclare(void*, CsdSchedQueue);

#if CMK_NODE_QUEUE_AVAILABLE
CsvDeclare(void*, CsdNodeQueue);
CsvDeclare(CmiNodeLock, CsdNodeQueueLock);
#endif
CpvDeclare(int,   CsdStopFlag);
CpvDeclare(int,   CsdLocalCounter);

CpvDeclare(int,   _urgentSend);

CmiNodeLock _smp_mutex;               /* for smp */

#if CMK_USE_IBVERBS | CMK_USE_IBUD
void *infi_CmiAlloc(int size);
void infi_CmiFree(void *ptr);
void infi_freeMultipleSend(void *ptr);
void infi_unregAndFreeMeta(void *ch);
#endif

#if CMK_SMP && CMK_BLUEGENEQ && SPECIFIC_PCQUEUE
void * CmiAlloc_bgq (int     size);
void   CmiFree_bgq  (void  * buf);
#endif

#if CMK_SMP && CMK_PPC_ATOMIC_QUEUE
void * CmiAlloc_ppcq (int     size);
void   CmiFree_ppcq  (void  * buf);
#endif

#if CMK_GRID_QUEUE_AVAILABLE
CpvDeclare(void *, CkGridObject);
CpvDeclare(void *, CsdGridQueue);
#endif

#if CMK_CRAYXE || CMK_CRAYXC
void* LrtsAlloc(int, int);
void  LrtsFree(void*);
#endif

CpvStaticDeclare(int, cmiMyPeIdle);
int CmiIsMyNodeIdle();

/*****************************************************************************
 *
 * Command-Line Argument (CLA) parsing routines.
 *
 *****************************************************************************/

static int usageChecked=0; /* set when argv has been searched for a usage request */
static int printUsage=0; /* if set, print command-line usage information */
static const char *CLAformatString="%20s %10s %s\n";

/** This little list of CLA's holds the argument descriptions until it's
   safe to print them--it's needed because the net- versions don't have 
   printf until they're pretty well started.
 */
typedef struct {
	const char *arg; /* Flag name, like "-foo"*/
	const char *param; /* Argument's parameter type, like "integer" or "none"*/
	const char *desc; /* Human-readable description of what it does */
} CLA;
static int CLAlistLen=0;
static int CLAlistMax=0;
static CLA *CLAlist=NULL;

/** Add this CLA */
static void CmiAddCLA(const char *arg,const char *param,const char *desc) {
	int i;
	if (CmiMyPe()!=0) return; /*Don't bother if we're not PE 0*/
	if (desc==NULL) return; /*It's an internal argument*/
	if (usageChecked) { /* Printf should work now */
		if (printUsage)
			CmiPrintf(CLAformatString,arg,param,desc);
	}
	else { /* Printf doesn't work yet-- just add to the list.
		This assumes the const char *'s are static references,
		which is probably reasonable. */
                CLA *temp;
		i=CLAlistLen++;
		if (CLAlistLen>CLAlistMax) { /*Grow the CLA list */
			CLAlistMax=16+2*CLAlistLen;
			temp=realloc(CLAlist,sizeof(CLA)*CLAlistMax);
                        if(temp != NULL) {
			  CLAlist=temp;
                        } else {
                          free(CLAlist);
                          CmiAbort("Reallocation failed for CLAlist\n");
                        }
		}
		CLAlist[i].arg=arg;
		CLAlist[i].param=param;
		CLAlist[i].desc=desc;
	}
}

/** Print out the stored list of CLA's */
static void CmiPrintCLAs(void) {
	int i;
	if (CmiMyPe()!=0) return; /*Don't bother if we're not PE 0*/
	CmiPrintf("Converse Machine Command-line Parameters:\n ");
	CmiPrintf(CLAformatString,"Option:","Parameter:","Description:");
	for (i=0;i<CLAlistLen;i++) {
		CLA *c=&CLAlist[i];
		CmiPrintf(CLAformatString,c->arg,c->param,c->desc);
	}
}

/**
 * Determines if command-line usage information should be printed--
 * that is, if a "-?", "-h", or "--help" flag is present.
 * Must be called after printf is setup.
 */
void CmiArgInit(char **argv) {
	int i;
	CmiLock(_smp_mutex);
	for (i=0;argv[i]!=NULL;i++)
	{
		if (0==strcmp(argv[i],"-?") ||
		    0==strcmp(argv[i],"-h") ||
		    0==strcmp(argv[i],"--help")) 
		{
			printUsage=1;
			/* Don't delete arg:  CmiDeleteArgs(&argv[i],1);
			  Leave it there for user program to see... */
			CmiPrintCLAs();
		}
	}
	if (CmiMyPe()==0) { /* Throw away list of stored CLA's */
		CLAlistLen=CLAlistMax=0;
		free(CLAlist); CLAlist=NULL;
	}
	usageChecked=1;
	CmiUnlock(_smp_mutex);
}

/** Return 1 if we're currently printing command-line usage information. */
int CmiArgGivingUsage(void) {
	return (CmiMyPe()==0) && printUsage;
}

/** Identifies the module that accepts the following command-line parameters */
void CmiArgGroup(const char *parentName,const char *groupName) {
	if (CmiArgGivingUsage()) {
		if (groupName==NULL) groupName=parentName; /* Start of a new group */
		CmiPrintf("\n%s Command-line Parameters:\n",groupName);
	}
}

/** Count the number of non-NULL arguments in list*/
int CmiGetArgc(char **argv)
{
	int i=0,argc=0;
	if (argv)
	while (argv[i++]!=NULL)
		argc++;
	return argc;
}

/** Return a new, heap-allocated copy of the argv array*/
char **CmiCopyArgs(char **argv)
{
	int argc=CmiGetArgc(argv);
	char **ret=(char **)malloc(sizeof(char *)*(argc+1));
	int i;
	for (i=0;i<=argc;i++)
		ret[i]=argv[i];
	return ret;
}

/** Delete the first k argument from the given list, shifting
all other arguments down by k spaces.
e.g., argv=={"a","b","c","d",NULL}, k==3 modifies
argv={"d",NULL,"c","d",NULL}
*/
void CmiDeleteArgs(char **argv,int k)
{
	int i=0;
	while ((argv[i]=argv[i+k])!=NULL)
		i++;
}

/** Find the given argment and string option in argv.
If the argument is present, set the string option and
delete both from argv.  If not present, return NULL.
e.g., arg=="-name" returns "bob" from
argv=={"a.out","foo","-name","bob","bar"},
and sets argv={"a.out","foo","bar"};
*/
int CmiGetArgStringDesc(char **argv,const char *arg,char **optDest,const char *desc)
{
	int i, found=0;
	char buf[256];
	CmiAddCLA(arg,"string",desc);
	for (i=0;argv[i]!=NULL;i++)
		if (0==strcmp(argv[i],arg)) { /*We found the argument*/
			if (!found) {
				if (argv[i+1]==NULL) CmiAbort("Argument not complete!");
				*optDest=argv[i+1];
				found = 1;
			}
			else { /* found == 1 which means users used this option more than once in the command line */
				snprintf(buf, 256, "Option %s is used more than once\n", argv[i]);
				CmiAbort(buf);
			}
			CmiDeleteArgs(&argv[i],2);
			i=i-1;
		}

	return found; /*return the result of the loop above : 0 -> not found, 1 ->found*/
}

int CmiGetArgString(char **argv,const char *arg,char **optDest) {
	return CmiGetArgStringDesc(argv,arg,optDest,"");
}

/** Find the given argument and floating-point option in argv.
Remove it and return 1; or return 0.
*/
int CmiGetArgDoubleDesc(char **argv,const char *arg,double *optDest,const char *desc) {
	char *number=NULL;
	CmiAddCLA(arg,"number",desc);
	if (!CmiGetArgStringDesc(argv,arg,&number,NULL)) return 0;
	if (1!=sscanf(number,"%lg",optDest)) return 0;
	return 1;
}
int CmiGetArgDouble(char **argv,const char *arg,double *optDest) {
	return CmiGetArgDoubleDesc(argv,arg,optDest,"");
}

/** Find the given argument and integer option in argv.
If the argument is present, parse and set the numeric option,
delete both from argv, and return 1. If not present, return 0.
e.g., arg=="-pack" matches argv=={...,"-pack","27",...},
argv=={...,"-pack0xf8",...}, and argv=={...,"-pack=0777",...};
but not argv=={...,"-packsize",...}.
*/
int CmiGetArgIntDesc(char **argv,const char *arg,int *optDest,const char *desc)
{
	int i, found = 0;
	char buf[256];

	int argLen=strlen(arg);
	CmiAddCLA(arg,"integer",desc);
	for (i=0;argv[i]!=NULL;i++) {
		if (0==strncmp(argv[i],arg,argLen)) {
			/*We *may* have found the argument*/
			const char *opt=NULL;
			int nDel=0;
			switch(argv[i][argLen]) {
			case 0: /* like "-p","27" */
				opt=argv[i+1]; nDel=2; break;
			case '=': /* like "-p=27" */
				opt=&argv[i][argLen+1]; nDel=1; break;
			case '-':case '+':
			case '0':case '1':case '2':case '3':case '4':
			case '5':case '6':case '7':case '8':case '9':
				/* like "-p27" */
				opt=&argv[i][argLen]; nDel=1; break;
			default:
				continue; /*False alarm-- skip it*/
			}
			if (opt==NULL) continue; /*False alarm*/

			if (!found) {
				if (sscanf(opt,"%i",optDest)<1) {
					/*Bad command line argument-- die*/
					fprintf(stderr,"Cannot parse %s option '%s' "
						"as an integer.\n",arg,opt);
					CmiAbort("Bad command-line argument\n");
				}
				found = 1;
			}
			else { /* found == 1 which means users used this option more than once in the command line */
				snprintf(buf, 256, "Option %s is used more than once\n", argv[i]);
				CmiAbort(buf);
			}

			CmiDeleteArgs(&argv[i],nDel);
			i = i - 1;
		}
	}
	return found; /*return the result of the loop above : 0 -> not found, 1 ->found*/
}
int CmiGetArgInt(char **argv,const char *arg,int *optDest) {
	return CmiGetArgIntDesc(argv,arg,optDest,"");
}

int CmiGetArgLongDesc(char **argv,const char *arg,CmiInt8 *optDest,const char *desc)
{
	int i, found = 0;
	char buf[256];

	int argLen=strlen(arg);
	CmiAddCLA(arg,"integer",desc);
	for (i=0;argv[i]!=NULL;i++)
		if (0==strncmp(argv[i],arg,argLen)) {/*We *may* have found the argument*/
			const char *opt=NULL;
			int nDel=0;
			switch(argv[i][argLen]) {
			case 0: /* like "-p","27" */
				opt=argv[i+1]; nDel=2; break;
			case '=': /* like "-p=27" */
				opt=&argv[i][argLen+1]; nDel=1; break;
			case '-':case '+':
			case '0':case '1':case '2':case '3':case '4':
			case '5':case '6':case '7':case '8':case '9':
				/* like "-p27" */
				opt=&argv[i][argLen]; nDel=1; break;
			default:
				continue; /*False alarm-- skip it*/
			}
			if (opt==NULL) continue; /*False alarm*/
			if (!found) {
				if (sscanf(opt,"%ld",optDest) < 1) {
					/*Bad command line argument-- die*/
					fprintf(stderr,"Cannot parse %s option '%s' "
						"as a long integer.\n",arg,opt);
					CmiAbort("Bad command-line argument\n");
				}
				found = 1;
			}
			else {/* found == 1 which means users used this option more than once in the command line */
				snprintf(buf, 256, "Option %s is used more than once\n", argv[i]);
				CmiAbort(buf);
			}

			CmiDeleteArgs(&argv[i],nDel);
			i = i-1;
		}
	return found; /*return the result of the loop above : 0 -> not found, 1 ->found*/
}
int CmiGetArgLong(char **argv,const char *arg,CmiInt8 *optDest) {
	return CmiGetArgLongDesc(argv,arg,optDest,"");
}

/** Find the given argument in argv.  If present, delete
it and return 1; if not present, return 0.
e.g., arg=="-foo" matches argv=={...,"-foo",...} but not
argv={...,"-foobar",...}.
*/
int CmiGetArgFlagDesc(char **argv,const char *arg,const char *desc)
{
	int i, found = 0;
	char buf[256];

	CmiAddCLA(arg,"",desc);
	for (i=0;argv[i]!=NULL;i++) {
		if (0==strcmp(argv[i],arg)) {/*We found the argument*/
			if (!found)
				found = 1;
			else { /* found == 1 which means users used this option more than once in the command line */
				snprintf(buf, 256, "Option %s is used more than once\n", argv[i]);
				CmiAbort(buf);
			}
			CmiDeleteArgs(&argv[i],1);
			i = i-1;
		}
	}
	return found; /*return the result of the loop above : 0 -> not found, 1 ->found*/
}
int CmiGetArgFlag(char **argv,const char *arg) {
	return CmiGetArgFlagDesc(argv,arg,"");
}

void CmiDeprecateArgInt(char **argv,const char *arg,const char *desc,const char *warning)
{
  int dummy = 0, found = CmiGetArgIntDesc(argv, arg, &dummy, desc);

  if (found)
    CmiPrintf(warning);
}

/*****************************************************************************
 *
 * Stack tracing routines.
 *
 *****************************************************************************/
#include "cmibacktrace.c"

/*
Convert "X(Y) Z" to "Y Z"-- remove text prior to first '(', and supress
the next parenthesis.  Operates in-place on the character data.
or Convert X(Y) to "Y" only, when trimname=1
*/
static char *_implTrimParenthesis(char *str, int trimname) {
  char *lParen=str, *ret=NULL, *rParen=NULL;
  while (*lParen!='(') {
    if (*lParen==0) return str; /* No left parenthesis at all. */
    lParen++;
  }
  /* now *lParen=='(', so trim it*/
  ret=lParen+1;
  rParen=ret;
  while (*rParen!=')') {
    if (*rParen==0) return ret; /* No right parenthesis at all. */
    rParen++;
  }
  /* now *rParen==')', so trim it*/
  *rParen=trimname?0:' ';
  return ret;  
}

/*
Return the text description of this trimmed routine name, if 
it's a system-generated routine where we should stop printing. 
This is probably overkill, but improves the appearance of callbacks.
*/
static const char* _implGetBacktraceSys(const char *name) {
  if (0==strncmp(name,"_call",5)) 
  { /*it might be something we're interested in*/
    if (0==strncmp(name,"_call_",6)) return "Call Entry Method";
    if (0==strncmp(name,"_callthr_",9)) return "Call Threaded Entry Method";
  }
  if (0==strncmp(name,"CthResume",9)) return "Resumed thread";
  if (0==strncmp(name,"qt_args",7)) return "Converse thread";
  
  return 0; /*ordinary user routine-- just print normally*/
}

/** Print out the names of these function pointers. */
void CmiBacktracePrint(void **retPtrs,int nLevels) {
  if (nLevels>0) {
    int i;
    char **names=CmiBacktraceLookup(retPtrs,nLevels);
    if (names==NULL) return;
    CmiPrintf("[%d] Stack Traceback:\n", CmiMyPe());
    for (i=0;i<nLevels;i++) {
      if (names[i] == NULL) continue;
      {
      const char *trimmed=_implTrimParenthesis(names[i], 0);
      const char *print=trimmed;
      const char *sys=_implGetBacktraceSys(print);
      if (sys) {
          CmiPrintf("  [%d] Charm++ Runtime: %s (%s)\n",i,sys,print);
          break; /*Stop when we hit Charm++ runtime.*/
      } else {
          CmiPrintf("  [%d:%d] %s\n",CmiMyPe(),i,print);
      }
     }
    }
    free(names);
  }
}

/* Print (to stdout) the names of the functions that have been 
   called up to this point. nSkip is the number of routines on the
   top of the stack to *not* print out. */
void CmiPrintStackTrace(int nSkip) {
#if CMK_USE_BACKTRACE
	int nLevels=max_stack;
	void *stackPtrs[max_stack];
	CmiBacktraceRecord(stackPtrs,1+nSkip,&nLevels);
	CmiBacktracePrint(stackPtrs,nLevels);
#endif
}

int CmiIsFortranLibraryCall() {
#if CMK_USE_BACKTRACE
  int ret = 0;
  int nLevels=9;
  void *stackPtrs[18];
  CmiBacktraceRecord(stackPtrs,1,&nLevels);
  if (nLevels>0) {
    int i;
    char **names=CmiBacktraceLookup(stackPtrs,nLevels);
    const char *trimmed;
    if (names==NULL) return 0;
    for (i=0;i<nLevels;i++) {
      if (names[i] == NULL) continue;
      trimmed=_implTrimParenthesis(names[i], 1);
      if (strncmp(trimmed, "for__", 5) == 0                /* ifort */
          || strncmp(trimmed, "_xlf", 4) == 0               /* xlf90 */
          || strncmp(trimmed, "_xlfBeginIO", 11) == 0 
          || strncmp(trimmed, "_gfortran_", 10) == 0 
	 )
          {  /* CmiPrintf("[%d] NAME:%s\n", CmiMyPe(), trimmed); */
             ret = 1; break; }
    }
    free(names);
  }
  return ret;
#else
  return 0;
#endif
}

/*****************************************************************************
 *
 * Statistics: currently, the following statistics are not updated by converse.
 *
 *****************************************************************************/

CpvDeclare(int, CstatsMaxChareQueueLength);
CpvDeclare(int, CstatsMaxForChareQueueLength);
CpvDeclare(int, CstatsMaxFixedChareQueueLength);
CpvStaticDeclare(int, CstatPrintQueueStatsFlag);
CpvStaticDeclare(int, CstatPrintMemStatsFlag);

void CstatsInit(argv)
char **argv;
{

#ifdef MEMMONITOR
  CpvInitialize(mmulong,MemoryUsage);
  CpvAccess(MemoryUsage) = 0;
  CpvInitialize(mmulong,HiWaterMark);
  CpvAccess(HiWaterMark) = 0;
  CpvInitialize(mmulong,ReportedHiWaterMark);
  CpvAccess(ReportedHiWaterMark) = 0;
  CpvInitialize(int,AllocCount);
  CpvAccess(AllocCount) = 0;
  CpvInitialize(int,BlocksAllocated);
  CpvAccess(BlocksAllocated) = 0;
#endif

  CpvInitialize(int, CstatsMaxChareQueueLength);
  CpvInitialize(int, CstatsMaxForChareQueueLength);
  CpvInitialize(int, CstatsMaxFixedChareQueueLength);
  CpvInitialize(int, CstatPrintQueueStatsFlag);
  CpvInitialize(int, CstatPrintMemStatsFlag);

  CpvAccess(CstatsMaxChareQueueLength) = 0;
  CpvAccess(CstatsMaxForChareQueueLength) = 0;
  CpvAccess(CstatsMaxFixedChareQueueLength) = 0;
  CpvAccess(CstatPrintQueueStatsFlag) = 0;
  CpvAccess(CstatPrintMemStatsFlag) = 0;

#if 0
  if (CmiGetArgFlagDesc(argv,"+mems", "Print memory statistics at shutdown"))
    CpvAccess(CstatPrintMemStatsFlag)=1;
  if (CmiGetArgFlagDesc(argv,"+qs", "Print queue statistics at shutdown"))
    CpvAccess(CstatPrintQueueStatsFlag)=1;
#endif
}

int CstatMemory(i)
int i;
{
  return 0;
}

int CstatPrintQueueStats()
{
  return CpvAccess(CstatPrintQueueStatsFlag);
}

int CstatPrintMemStats()
{
  return CpvAccess(CstatPrintMemStatsFlag);
}

/*****************************************************************************
 *
 * Cmi handler registration
 *
 *****************************************************************************/

CpvDeclare(CmiHandlerInfo*, CmiHandlerTable);
CpvStaticDeclare(int  , CmiHandlerCount);
CpvStaticDeclare(int  , CmiHandlerLocal);
CpvStaticDeclare(int  , CmiHandlerGlobal);
CpvDeclare(int,         CmiHandlerMax);

static void CmiExtendHandlerTable(int atLeastLen) {
    int max = CpvAccess(CmiHandlerMax);
    int newmax = (atLeastLen+(atLeastLen>>2)+32);
    int bytes = max*sizeof(CmiHandlerInfo);
    int newbytes = newmax*sizeof(CmiHandlerInfo);
    CmiHandlerInfo *nu = (CmiHandlerInfo*)malloc(newbytes);
    CmiHandlerInfo *tab = CpvAccess(CmiHandlerTable);
    _MEMCHECK(nu);
    memcpy(nu, tab, bytes);
    memset(((char *)nu)+bytes, 0, (newbytes-bytes));
    free(tab); tab=nu;
    CpvAccess(CmiHandlerTable) = tab;
    CpvAccess(CmiHandlerMax) = newmax;
}

void CmiNumberHandler(int n, CmiHandler h)
{
  CmiHandlerInfo *tab;
  if (n >= CpvAccess(CmiHandlerMax)) CmiExtendHandlerTable(n);
  tab = CpvAccess(CmiHandlerTable);
  tab[n].hdlr = (CmiHandlerEx)h; /* LIE!  This assumes extra pointer will be ignored!*/
  tab[n].userPtr = 0;
}
void CmiNumberHandlerEx(int n, CmiHandlerEx h,void *userPtr) {
  CmiHandlerInfo *tab;
  if (n >= CpvAccess(CmiHandlerMax)) CmiExtendHandlerTable(n);
  tab = CpvAccess(CmiHandlerTable);
  tab[n].hdlr = h;
  tab[n].userPtr=userPtr;
}

#if CMI_LOCAL_GLOBAL_AVAILABLE /*Leave room for local and global handlers*/
#  define DIST_BETWEEN_HANDLERS 3
#else /*No local or global handlers; ordinary handlers are back-to-back*/
#  define DIST_BETWEEN_HANDLERS 1
#endif

int CmiRegisterHandler(CmiHandler h)
{
  int Count = CpvAccess(CmiHandlerCount);
  CmiNumberHandler(Count, h);
  CpvAccess(CmiHandlerCount) = Count+DIST_BETWEEN_HANDLERS;
  return Count;
}
int CmiRegisterHandlerEx(CmiHandlerEx h,void *userPtr)
{
  int Count = CpvAccess(CmiHandlerCount);
  CmiNumberHandlerEx(Count, h, userPtr);
  CpvAccess(CmiHandlerCount) = Count+DIST_BETWEEN_HANDLERS;
  return Count;
}

#if CMI_LOCAL_GLOBAL_AVAILABLE
int CmiRegisterHandlerLocal(h)
CmiHandler h;
{
  int Local = CpvAccess(CmiHandlerLocal);
  CmiNumberHandler(Local, h);
  CpvAccess(CmiHandlerLocal) = Local+3;
  return Local;
}

int CmiRegisterHandlerGlobal(h)
CmiHandler h;
{
  int Global = CpvAccess(CmiHandlerGlobal);
  if (CmiMyPe()!=0) 
    CmiError("CmiRegisterHandlerGlobal must only be called on PE 0.\n");
  CmiNumberHandler(Global, h);
  CpvAccess(CmiHandlerGlobal) = Global+3;
  return Global;
}
#endif

static void _cmiZeroHandler(void *msg) {
	CmiAbort("Converse zero handler executed-- was a message corrupted?\n");
}

static void CmiHandlerInit()
{
  CpvInitialize(CmiHandlerInfo *, CmiHandlerTable);
  CpvInitialize(int         , CmiHandlerCount);
  CpvInitialize(int         , CmiHandlerLocal);
  CpvInitialize(int         , CmiHandlerGlobal);
  CpvInitialize(int         , CmiHandlerMax);
  CpvAccess(CmiHandlerCount)  = 0;
  CpvAccess(CmiHandlerLocal)  = 1;
  CpvAccess(CmiHandlerGlobal) = 2;
  CpvAccess(CmiHandlerMax) = 0; /* Table will be extended on the first registration*/
  CpvAccess(CmiHandlerTable) = NULL;
  CmiRegisterHandler((CmiHandler)_cmiZeroHandler);
}


/******************************************************************************
 *
 * CmiTimer
 *
 * Here are two possible implementations of CmiTimer.  Some machines don't
 * select either, and define the timer in machine.c instead.
 *
 *****************************************************************************/

#if CMK_HAS_ASCTIME

char *CmiPrintDate()
{
  struct tm *local;
  time_t t;

  t = time(NULL);
  local = localtime(&t);
  return asctime(local);
}

#else

char *CmiPrintDate()
{
  return "N/A";
}

#endif

static int _absoluteTime = 0;

#if CMK_TIMER_USE_TIMES

CpvStaticDeclare(double, clocktick);
CpvStaticDeclare(int,inittime_wallclock);
CpvStaticDeclare(int,inittime_virtual);

int CmiTimerIsSynchronized()
{
  return 0;
}

int CmiTimerAbsolute()
{
  return 0;
}

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return CpvAccess(inittime_wallclock);
}

void CmiTimerInit(char **argv)
{
  struct tms temp;
  CpvInitialize(double, clocktick);
  CpvInitialize(int, inittime_wallclock);
  CpvInitialize(int, inittime_virtual);
  CpvAccess(inittime_wallclock) = times(&temp);
  CpvAccess(inittime_virtual) = temp.tms_utime + temp.tms_stime;
  CpvAccess(clocktick) = 1.0 / (sysconf(_SC_CLK_TCK));
}

double CmiWallTimer()
{
  struct tms temp;
  double currenttime;
  int now;

  now = times(&temp);
  currenttime = (now - CpvAccess(inittime_wallclock)) * CpvAccess(clocktick);
  return (currenttime);
}

double CmiCpuTimer()
{
  struct tms temp;
  double currenttime;
  int now;

  times(&temp);
  now = temp.tms_stime + temp.tms_utime;
  currenttime = (now - CpvAccess(inittime_virtual)) * CpvAccess(clocktick);
  return (currenttime);
}

double CmiTimer()
{
  return CmiCpuTimer();
}

#endif

#if CMK_TIMER_USE_GETRUSAGE

#if CMK_SMP
# if CMK_HAS_RUSAGE_THREAD
#define RUSAGE_WHO        1   /* RUSAGE_THREAD, only in latest Linux kernels */
#else
#undef RUSAGE_WHO
#endif
#else
#define RUSAGE_WHO        0
#endif

static double inittime_wallclock;
CpvStaticDeclare(double, inittime_virtual);

int CmiTimerIsSynchronized()
{
  return 0;
}

int CmiTimerAbsolute()
{
  return _absoluteTime;
}

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return inittime_wallclock;
}

void CmiTimerInit(char **argv)
{
  struct timeval tv;
  struct rusage ru;
  CpvInitialize(double, inittime_virtual);

  int tmptime = CmiGetArgFlagDesc(argv,"+useAbsoluteTime", "Use system's absolute time as wallclock time.");
  if(CmiMyRank() == 0) _absoluteTime = tmptime;   /* initialize only  once */
#if !(__FAULT__)
  /* try to synchronize calling barrier */
  CmiBarrier();
  CmiBarrier();
  CmiBarrier();
#endif
if(CmiMyRank() == 0) /* initialize only  once */
  {
    gettimeofday(&tv,0);
    inittime_wallclock = (tv.tv_sec * 1.0) + (tv.tv_usec*0.000001);
#ifndef RUSAGE_WHO
    CpvAccess(inittime_virtual) = inittime_wallclock;
#else
    getrusage(RUSAGE_WHO, &ru); 
    CpvAccess(inittime_virtual) =
      (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
      (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);
#endif
  }

#if !(__FAULT__)
  CmiBarrier();
/*  CmiBarrierZero(); */
#endif
}

double CmiCpuTimer()
{
#ifndef RUSAGE_WHO
  return CmiWallTimer();
#else
  struct rusage ru;
  double currenttime;

  getrusage(RUSAGE_WHO, &ru);
  currenttime =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);
  
  return currenttime - CpvAccess(inittime_virtual);
#endif
}

static double lastT = -1.0;

double CmiWallTimer()
{
  struct timeval tv;
  double currenttime;

  gettimeofday(&tv,0);
  currenttime = (tv.tv_sec * 1.0) + (tv.tv_usec * 0.000001);
#if CMK_ERROR_CHECKING
  if (lastT > 0.0 && currenttime < lastT) {
    currenttime = lastT;
  }
  lastT = currenttime;
#endif
  return _absoluteTime?currenttime:currenttime - inittime_wallclock;
}

double CmiTimer()
{
  return CmiCpuTimer();
}

#endif

#if CMK_TIMER_USE_RDTSC

static double readMHz(void)
{
  double x;
  char str[1000];
  char buf[100];
  FILE *fp;
  CmiLock(_smp_mutex);
  fp = fopen("/proc/cpuinfo", "r");
  if (fp != NULL)
  while(fgets(str, 1000, fp)!=0) {
    if(sscanf(str, "cpu MHz%[^:]",buf)==1)
    {
      char *s = strchr(str, ':'); s=s+1;
      sscanf(s, "%lf", &x);
      fclose(fp);
      CmiUnlock(_smp_mutex);
      return x;
    }
  }
  CmiUnlock(_smp_mutex);
  CmiAbort("Cannot read CPU MHz from /proc/cpuinfo file.");
  return 0.0;
}

double _cpu_speed_factor;
CpvStaticDeclare(double, inittime_virtual);
CpvStaticDeclare(double, inittime_walltime);

double  CmiStartTimer(void)
{
  return CpvAccess(inittime_walltime);
}

double CmiInitTime()
{
  return CpvAccess(inittime_walltime);
}

void CmiTimerInit(char **argv)
{
  struct rusage ru;

  CmiBarrier();
  CmiBarrier();

  _cpu_speed_factor = 1.0/(readMHz()*1.0e6); 
  rdtsc(); rdtsc(); rdtsc(); rdtsc(); rdtsc();
  CpvInitialize(double, inittime_walltime);
  CpvAccess(inittime_walltime) = CmiWallTimer();
  CpvInitialize(double, inittime_virtual);
  getrusage(0, &ru); 
  CpvAccess(inittime_virtual) =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);

  CmiBarrierZero();
}

double CmiCpuTimer()
{
  struct rusage ru;
  double currenttime;

  getrusage(0, &ru);
  currenttime =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);
  return currenttime - CpvAccess(inittime_virtual);
}

#endif

#if CMK_BLUEGENEL || CMK_BLUEGENEP
#include "dcopy.h"
#endif

#if CMK_TIMER_USE_BLUEGENEL

#include "rts.h"

#if 0 
#define SPRN_TBRL 0x10C  /* Time Base Read Lower Register (user & sup R/O) */
#define SPRN_TBRU 0x10D  /* Time Base Read Upper Register (user & sup R/O) */
#define SPRN_PIR  0x11E  /* CPU id */

static inline unsigned long long BGLTimebase(void)
{
  unsigned volatile u1, u2, lo;
  union
  {
    struct { unsigned hi, lo; } w;
    unsigned long long d;
  } result;
                                                                                
  do {
    asm volatile ("mfspr %0,%1" : "=r" (u1) : "i" (SPRN_TBRU));
    asm volatile ("mfspr %0,%1" : "=r" (lo) : "i" (SPRN_TBRL));
    asm volatile ("mfspr %0,%1" : "=r" (u2) : "i" (SPRN_TBRU));
  } while (u1!=u2);
                                                                                
  result.w.lo = lo;
  result.w.hi = u2;
  return result.d;
}
#endif

static unsigned long long inittime_wallclock = 0;
CpvStaticDeclare(double, clocktick);

int CmiTimerIsSynchronized()
{
  return 0;
}

int CmiTimerAbsolute()
{
  return 1;
}

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return inittime_wallclock;
}

void CmiTimerInit(char **argv)
{
  BGLPersonality dst;
  CpvInitialize(double, clocktick);
  int size = sizeof(BGLPersonality);
  rts_get_personality(&dst, size);
  CpvAccess(clocktick) = 1.0 / dst.clockHz;

  /* try to synchronize calling barrier */
  CmiBarrier();
  CmiBarrier();
  CmiBarrier();

  /* inittime_wallclock = rts_get_timebase(); */
  inittime_wallclock = 0.0;    /* use bgl absolute time */
}

double CmiWallTimer()
{
  unsigned long long currenttime;
  currenttime = rts_get_timebase();
  return CpvAccess(clocktick)*(currenttime-inittime_wallclock);
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiWallTimer();
}

#endif

#if CMK_TIMER_USE_BLUEGENEP  /* This module just compiles with GCC charm. */

int CmiTimerAbsolute()
{
  return 0;
}

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return 0.0;
}

void CmiTimerInit(char **argv) {}

#include "dcmf.h"

double CmiWallTimer () {
  return DCMF_Timer();
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiWallTimer();
}
#endif


#if CMK_TIMER_USE_BLUEGENEQ  /* This module just compiles with GCC charm. */

CpvStaticDeclare(unsigned long, inittime);
CpvStaticDeclare(double, clocktick);

int CmiTimerIsSynchronized()
{
  return 1;
}

int CmiTimerAbsolute()
{
  return 0;
}

#include "hwi/include/bqc/A2_inlines.h"
#include "spi/include/kernel/process.h"

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return CpvAccess(inittime);
}

void CmiTimerInit(char **argv)
{
  CpvInitialize(double, clocktick);
  CpvInitialize(unsigned long, inittime);

  Personality_t  pers;
  Kernel_GetPersonality(&pers, sizeof(pers));
  uint32_t clockMhz = pers.Kernel_Config.FreqMHz;
  CpvAccess(clocktick) = 1.0 / (clockMhz * 1e6); 

  /*fprintf(stderr, "Blue Gene/Q running at clock speed of %d Mhz\n", clockMhz);*/

  /* try to synchronize calling barrier */
#if !(__FAULT__)
  CmiBarrier();
  CmiBarrier();
  CmiBarrier();
#endif
  CpvAccess(inittime) = GetTimeBase (); 
}

double CmiWallTimer()
{
  unsigned long long currenttime;
  currenttime = GetTimeBase();
  return CpvAccess(clocktick)*(currenttime-CpvAccess(inittime));
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiWallTimer();
}

#endif


#if CMK_TIMER_USE_PPC64

#include <sys/time.h>
#include <endian.h>

#define SPRN_TBRU 0x10D
#define SPRN_TBRL 0x10C

CpvStaticDeclare(uint64_t, inittime);
CpvStaticDeclare(double, clocktick);

int CmiTimerIsSynchronized()
{
  return 1;
}

int CmiTimerAbsolute()
{
  return 0;
}

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return CpvAccess(inittime);
}

static inline uint64_t PPC64_TimeBase()
{
  unsigned temp;
  union
  {
#if __BYTE_ORDER  == __LITTLE_ENDIAN
    struct { unsigned lo, hi; } w;
#else
#warning "PPC64 Is BigEndian"
    struct { unsigned hi, lo; } w;
#endif
    uint64_t d;
  } result;

  do {
    asm volatile ("mfspr %0,%1" : "=r" (temp)        : "i" (SPRN_TBRU));
    asm volatile ("mfspr %0,%1" : "=r" (result.w.lo) : "i" (SPRN_TBRL));
    asm volatile ("mfspr %0,%1" : "=r" (result.w.hi) : "i" (SPRN_TBRU));
  }
  while (temp != result.w.hi);

  return result.d;
}

uint64_t __micro_timer () {
  struct timeval tv;
  gettimeofday( &tv, 0 );
  return tv.tv_sec * 1000000ULL + tv.tv_usec;
}

void CmiTimerInit(char **argv)
{
  CpvInitialize(double, clocktick);
  CpvInitialize(unsigned long, inittime);

  //Initialize PPC64 timers

  uint64_t sampleTime = 100ULL; //sample time in usec
  uint64_t timeStart = 0ULL, timeStop = 0ULL;
  uint64_t startBase = 0ULL, endBase = 0ULL;
  uint64_t overhead = 0ULL, tbf = 0ULL, tbi = 0ULL;
  uint64_t ticks = 0ULL;
  int      iter = 0ULL;

  do {
    tbi = PPC64_TimeBase();
    tbf = PPC64_TimeBase();
    tbi = PPC64_TimeBase();
    tbf = PPC64_TimeBase();

    overhead = tbf - tbi;
    timeStart = __micro_timer();

    //wait for system time to change
    while (__micro_timer() == timeStart)
      timeStart = __micro_timer();

    while (1) {
      timeStop = __micro_timer();
      if ((timeStop - timeStart) > 1) {
        startBase = PPC64_TimeBase();
        break;
      }
    }
    timeStart = timeStop;

    while (1) {
      timeStop = __micro_timer();
      if ((timeStop - timeStart) > sampleTime) {
        endBase = PPC64_TimeBase();
        break;
      }
    }

    ticks = ((endBase - startBase) + (overhead));
    iter++;
    if (iter == 10ULL)
      CmiAbort("Warning: unable to initialize high resolution timer.\n");

  } while (endBase <= startBase);

  CpvAccess (clocktick) = (1e-6) / ((double)ticks/(double)sampleTime);

  /* try to synchronize calling barrier */
#if !(__FAULT__)
  CmiBarrier();
  CmiBarrier();
  CmiBarrier();
#endif
  CpvAccess(inittime) = PPC64_TimeBase ();
}

double CmiWallTimer()
{
  uint64_t currenttime;
  currenttime = PPC64_TimeBase();
  return CpvAccess(clocktick)*(currenttime-CpvAccess(inittime));
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiWallTimer();
}

#endif


#if CMK_TIMER_USE_WIN32API

CpvStaticDeclare(double, inittime_wallclock);
CpvStaticDeclare(double, inittime_virtual);

double CmiStartTimer()
{
  return 0.0;
}

int CmiTimerAbsolute()
{
  return 0;
}

double CmiInitTime()
{
  return CpvAccess(inittime_wallclock);
}

void CmiTimerInit(char **argv)
{
#ifdef __CYGWIN__
	struct timeb tv;
#else
	struct _timeb tv;
#endif
	clock_t       ru;

	CpvInitialize(double, inittime_wallclock);
	CpvInitialize(double, inittime_virtual);
	_ftime(&tv);
	CpvAccess(inittime_wallclock) = tv.time*1.0 + tv.millitm*0.001;
	ru = clock();
	CpvAccess(inittime_virtual) = ((double) ru)/CLOCKS_PER_SEC;
}

double CmiCpuTimer()
{
	clock_t ru;
	double currenttime;

	ru = clock();
	currenttime = (double) ru/CLOCKS_PER_SEC;

	return currenttime - CpvAccess(inittime_virtual);
}

double CmiWallTimer()
{
#ifdef __CYGWIN__
	struct timeb tv;
#else
	struct _timeb tv;
#endif
	double currenttime;

	_ftime(&tv);
	currenttime = tv.time*1.0 + tv.millitm*0.001;

	return currenttime - CpvAccess(inittime_wallclock);
}
	

double CmiTimer()
{
	return CmiCpuTimer();
}

#endif

#if CMK_TIMER_USE_RTC

#if __crayx1
 /* For _rtc() on Cray X1 */
#include <intrinsics.h>
#endif

static double clocktick;
CpvStaticDeclare(long long, inittime_wallclock);

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return CpvAccess(inittime_wallclock);
}

void CmiTimerInit(char **argv)
{
  CpvInitialize(long long, inittime_wallclock);
  CpvAccess(inittime_wallclock) = _rtc();
  clocktick = 1.0 / (double)(sysconf(_SC_SV2_USER_TIME_RATE));
}

int CmiTimerAbsolute()
{
  return 0;
}

double CmiWallTimer()
{
  long long now;

  now = _rtc();
  return (clocktick * (now - CpvAccess(inittime_wallclock)));
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiCpuTimer();
}

#endif

#if CMK_TIMER_USE_AIX_READ_TIME

#include <sys/time.h>

static timebasestruct_t inittime_wallclock;
static double clocktick;
CpvStaticDeclare(double, inittime_virtual);

double CmiStartTimer()
{
  return 0.0;
}

double CmiInitTime()
{
  return inittime_wallclock;
}

void CmiTimerInit(char **argv)
{
  struct rusage ru;

  if (CmiMyRank() == 0) {
    read_wall_time(&inittime_wallclock, TIMEBASE_SZ);
    time_base_to_time(&inittime_wallclock, TIMEBASE_SZ);
  }

  CpvInitialize(double, inittime_virtual);
  getrusage(0, &ru);
  CpvAccess(inittime_virtual) =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);
}

int CmiTimerAbsolute()
{
  return 0;
}

double CmiWallTimer()
{
  int secs, n_secs;
  double curt;
  timebasestruct_t now;
  read_wall_time(&now, TIMEBASE_SZ);
  time_base_to_time(&now, TIMEBASE_SZ);

  secs = now.tb_high - inittime_wallclock.tb_high;
  n_secs = now.tb_low - inittime_wallclock.tb_low;
  if (n_secs < 0)  {
    secs--;
    n_secs += 1000000000;
  }
  curt = secs*1.0 + n_secs*1e-9;
  return curt;
}

double CmiCpuTimer()
{
  struct rusage ru;
  double currenttime;

  getrusage(0, &ru);
  currenttime =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);
  return currenttime - CpvAccess(inittime_virtual);
}

double CmiTimer()
{
  return CmiWallTimer();
}

#endif

#ifndef CMK_USE_SPECIAL_MESSAGE_QUEUE_CHECK
/** Return 1 if our outgoing message queue 
   for this node is longer than this many bytes. */
int CmiLongSendQueue(int forNode,int longerThanBytes) {
  return 0;
}
#endif

#if CMK_SIGNAL_USE_SIGACTION
#include <signal.h>
void CmiSignal(int sig1, int sig2, int sig3, void (*handler)())
{
  struct sigaction in, out ;
  in.sa_handler = handler;
  sigemptyset(&in.sa_mask);
  if (sig1) sigaddset(&in.sa_mask, sig1);
  if (sig2) sigaddset(&in.sa_mask, sig2);
  if (sig3) sigaddset(&in.sa_mask, sig3);
  in.sa_flags = 0;
  if (sig1) if (sigaction(sig1, &in, &out)<0) exit(1);
  if (sig2) if (sigaction(sig2, &in, &out)<0) exit(1);
  if (sig3) if (sigaction(sig3, &in, &out)<0) exit(1);
}
#endif

#if CMK_SIGNAL_USE_SIGACTION_WITH_RESTART
#include <signal.h>
void CmiSignal(sig1, sig2, sig3, handler)
int sig1, sig2, sig3;
void (*handler)();
{
  struct sigaction in, out ;
  in.sa_handler = handler;
  sigemptyset(&in.sa_mask);
  if (sig1) sigaddset(&in.sa_mask, sig1);
  if (sig2) sigaddset(&in.sa_mask, sig2);
  if (sig3) sigaddset(&in.sa_mask, sig3);
  in.sa_flags = SA_RESTART;
  if (sig1) if (sigaction(sig1, &in, &out)<0) exit(1);
  if (sig2) if (sigaction(sig2, &in, &out)<0) exit(1);
  if (sig3) if (sigaction(sig3, &in, &out)<0) exit(1);
}
#endif

/** 
 *  @addtogroup ConverseScheduler
 *  @{
 */

/*****************************************************************************
 * 
 * The following is the CsdScheduler function.  A common
 * implementation is provided below.  The machine layer can provide an
 * alternate implementation if it so desires.
 *
 * void CmiDeliversInit()
 *
 *      - CmiInit promises to call this before calling CmiDeliverMsgs
 *        or any of the other functions in this section.
 *
 * int CmiDeliverMsgs(int maxmsgs)
 *
 *      - CmiDeliverMsgs will retrieve up to maxmsgs that were transmitted
 *        with the Cmi, and will invoke their handlers.  It does not wait
 *        if no message is unavailable.  Instead, it returns the quantity
 *        (maxmsgs-delivered), where delivered is the number of messages it
 *        delivered.
 *
 * void CmiDeliverSpecificMsg(int handlerno)
 *
 *      - Waits for a message with the specified handler to show up, then
 *        invokes the message's handler.  Note that unlike CmiDeliverMsgs,
 *        This function _does_ wait.
 *
 * For this common implementation to work, the machine layer must provide the
 * following:
 *
 * void *CmiGetNonLocal()
 *
 *      - returns a message just retrieved from some other PE, not from
 *        local.  If no such message exists, returns 0.
 *
 * CpvExtern(CdsFifo, CmiLocalQueue);
 *
 *      - a FIFO queue containing all messages from the local processor.
 *
 *****************************************************************************/

void CsdBeginIdle(void)
{
  CcdCallBacks();
#if CMK_TRACE_ENABLED && CMK_PROJECTOR
  _LOG_E_PROC_IDLE(); 	/* projector */
#endif

  CpvAccess(cmiMyPeIdle) = 1;

  CcdRaiseCondition(CcdPROCESSOR_BEGIN_IDLE) ;
}

void CsdStillIdle(void)
{
  CcdRaiseCondition(CcdPROCESSOR_STILL_IDLE);
}

void CsdEndIdle(void)
{
#if CMK_TRACE_ENABLED && CMK_PROJECTOR
  _LOG_E_PROC_BUSY(); 	/* projector */
#endif

  CpvAccess(cmiMyPeIdle) = 0;

  CcdRaiseCondition(CcdPROCESSOR_BEGIN_BUSY) ;
}

extern int _exitHandlerIdx;

/** Takes a message and calls its corresponding handler. */
void CmiHandleMessage(void *msg)
{
/* this is wrong because it counts the Charm++ messages in sched queue
 	CpvAccess(cQdState)->mProcessed++;
*/
	CmiHandlerInfo *h;
#if CMK_TRACE_ENABLED && CMK_PROJECTOR
	CmiUInt2 handler=CmiGetHandler(msg); /* Save handler for use after msg is gone */
	_LOG_E_HANDLER_BEGIN(handler); /* projector */
	/* setMemoryStatus(1) */ /* charmdebug */
#endif

/*
	FAULT_EVAC
*/
/*	if((!CpvAccess(_validProcessors)[CmiMyPe()]) && handler != _exitHandlerIdx){
		return;
	}*/
	
        MESSAGE_PHASE_CHECK(msg)

	h=&CmiGetHandlerInfo(msg);
	(h->hdlr)(msg,h->userPtr);
#if CMK_TRACE_ENABLED
	/* setMemoryStatus(0) */ /* charmdebug */
	//_LOG_E_HANDLER_END(handler); 	/* projector */
#endif
}

#if CMK_CMIDELIVERS_USE_COMMON_CODE

void CmiDeliversInit()
{
}

int CmiDeliverMsgs(int maxmsgs)
{
  return CsdScheduler(maxmsgs);
}

#if CMK_OBJECT_QUEUE_AVAILABLE
CpvDeclare(void *, CsdObjQueue);
#endif

void CsdSchedulerState_new(CsdSchedulerState_t *s)
{
#if CMK_OBJECT_QUEUE_AVAILABLE
	s->objQ=CpvAccess(CsdObjQueue);
#endif
	s->localQ=CpvAccess(CmiLocalQueue);
	s->schedQ=CpvAccess(CsdSchedQueue);
	s->localCounter=&(CpvAccess(CsdLocalCounter));
#if CMK_NODE_QUEUE_AVAILABLE
	s->nodeQ=CsvAccess(CsdNodeQueue);
	s->nodeLock=CsvAccess(CsdNodeQueueLock);
#endif
#if CMK_GRID_QUEUE_AVAILABLE
	s->gridQ=CpvAccess(CsdGridQueue);
#endif
}


/** Dequeue and return the next message from the unprocessed message queues.
 *
 * This function encapsulates the multiple queues that exist for holding unprocessed
 * messages and the rules for the order in which to check them. There are five (5)
 * different Qs that converse uses to store and retrieve unprocessed messages. These
 * are:
 *     Q Purpose                  Type      internal DeQ logic
 * -----------------------------------------------------------
 * - PE offnode                   pcQ             FIFO
 * - PE onnode                    CkQ             FIFO
 * - Node offnode                 pcQ             FIFO
 * - Node onnode                  prioQ           prio-based
 * - Scheduler                    prioQ           prio-based
 *
 * The PE queues hold messages that are destined for a specific PE. There is one such
 * queue for every PE within a charm node. The node queues hold messages that are
 * destined to that node. There is only one of each node queue within a charm node.
 * Finally there is also a charm++ message queue for each PE.
 *
 * The offnode queues are meant for holding messages that arrive from outside the
 * node. The onnode queues hold messages that are generated within the same charm
 * node.
 *
 * The PE and node level offnode queues are accessed via functions CmiGetNonLocal()
 * and CmiGetNonLocalNodeQ(). These are implemented separately by each machine layer
 * and hide the implementation specifics for each layer.
 *
 * The PE onnode queue is implemented as a FIFO CkQ and is initialized via a call to
 * CdsFifo_Create(). The node local queue and the scheduler queue are both priority
 * queues. They are initialized via calls to CqsCreate() which gives each of them
 * three separate internal queues for different priority ranges (-ve, 0 and +ve).
 * Access to these queues is via pointers stored in the struct CsdSchedulerState that
 * is passed into this function.
 *
 * The order in which these queues are checked is described below. The function
 * proceeds to the next queue in the list only if it does not find any messages in
 * the current queue. The first message that is found is returned, terminating the
 * call.
 * (1) offnode queue for this PE
 * (2) onnode queue for this PE
 * (3) offnode queue for this node
 * (4) highest priority msg from onnode queue or scheduler queue
 *
 * @note: Across most (all?) machine layers, the two GetNonLocal functions simply
 * access (after observing adequate locking rigor) structs representing the scheduler
 * state, to dequeue from the queues stored within them. The structs (CmiStateStruct
 * and CmiNodeStateStruct) implement these queues as \ref Machine "pc (producer-consumer)
 * queues". The functions also perform other necessary actions like PumpMsgs() etc.
 *
 */
void *CsdNextMessage(CsdSchedulerState_t *s) {
	void *msg;
	if((*(s->localCounter))-- >0)
	  {
              /* This avoids a race condition with migration detected by megatest*/
              msg=CdsFifo_Dequeue(s->localQ);
              if (msg!=NULL)
		{
		  CpvAccess(cQdState)->mProcessed++;
		  return msg;	    
		}
              CqsDequeue(s->schedQ,(void **)&msg);
              if (msg!=NULL) return msg;
	  }
	
	*(s->localCounter)=CsdLocalMax;
	if ( NULL!=(msg=CmiGetNonLocal()) || 
	     NULL!=(msg=CdsFifo_Dequeue(s->localQ)) ) {
            CpvAccess(cQdState)->mProcessed++;
            return msg;
        }
#if CMK_GRID_QUEUE_AVAILABLE
	/*#warning "CsdNextMessage: CMK_GRID_QUEUE_AVAILABLE" */
	CqsDequeue (s->gridQ, (void **) &msg);
	if (msg != NULL) {
	  return (msg);
	}
#endif
#if CMK_NODE_QUEUE_AVAILABLE
	/*#warning "CsdNextMessage: CMK_NODE_QUEUE_AVAILABLE" */
	if (NULL!=(msg=CmiGetNonLocalNodeQ())) return msg;
	if (!CqsEmpty(s->nodeQ)
	 && CqsPrioGT(CqsGetPriority(s->schedQ),
		       CqsGetPriority(s->nodeQ))) {
	  if(CmiTryLock(s->nodeLock) == 0) {
	    CqsDequeue(s->nodeQ,(void **)&msg);
	    CmiUnlock(s->nodeLock);
	    if (msg!=NULL) return msg;
	  }
	}
#endif
#if CMK_OBJECT_QUEUE_AVAILABLE
	/*#warning "CsdNextMessage: CMK_OBJECT_QUEUE_AVAILABLE"   */
	if (NULL!=(msg=CdsFifo_Dequeue(s->objQ))) {
          return msg;
        }
#endif
        if(!CsdLocalMax) {
	  CqsDequeue(s->schedQ,(void **)&msg);
          if (msg!=NULL) return msg;	    
        }

        return NULL;
}


void *CsdNextLocalNodeMessage(CsdSchedulerState_t *s) {
	void *msg;
#if CMK_NODE_QUEUE_AVAILABLE
	/*#warning "CsdNextMessage: CMK_NODE_QUEUE_AVAILABLE" */
	/*if (NULL!=(msg=CmiGetNonLocalNodeQ())) return msg;*/
	if (!CqsEmpty(s->nodeQ))
	{
	  CmiLock(s->nodeLock);
	  CqsDequeue(s->nodeQ,(void **)&msg);
	  CmiUnlock(s->nodeLock);
	  if (msg!=NULL) return msg;
	}
#endif
	return NULL;

}

int CsdScheduler(int maxmsgs)
{
	if (maxmsgs<0) CsdScheduleForever();	
	else if (maxmsgs==0)
		CsdSchedulePoll();
	else /*(maxmsgs>0)*/ 
		return CsdScheduleCount(maxmsgs);
	return 0;
}

/*Declare the standard scheduler housekeeping*/
#define SCHEDULE_TOP \
      void *msg;\
      int *CsdStopFlag_ptr = &CpvAccess(CsdStopFlag); \
      int cycle = CpvAccess(CsdStopFlag); \
      CsdSchedulerState_t state;\
      CsdSchedulerState_new(&state);

/*A message is available-- process it*/
#define SCHEDULE_MESSAGE \
      CmiHandleMessage(msg);\
      if (*CsdStopFlag_ptr != cycle) break;

/*No message available-- go (or remain) idle*/
#define SCHEDULE_IDLE \
      if (!isIdle) {isIdle=1;CsdBeginIdle();}\
      else CsdStillIdle();\
      if (*CsdStopFlag_ptr != cycle) {\
	CsdEndIdle();\
	break;\
      }

/*
	EVAC
*/
extern void CkClearAllArrayElements();


extern void machine_OffloadAPIProgress();

#if CMK_OUT_OF_CORE
OOCCheckFn OOCPrivateCheck = NULL;
OOCProcessFn OOCPrivateProcess = NULL;
OOCCheckFn OOCNodeCheck = NULL;
OOCProcessFn OOCNodeProcess = NULL;
#endif

/** The main scheduler loop that repeatedly executes messages from a queue, forever. */
void CsdScheduleForever(void)
{
  #if CMK_CELL
    #define CMK_CELL_PROGRESS_FREQ  96  /* (MSG-Q Entries x1.5) */
    int progressCount = CMK_CELL_PROGRESS_FREQ;
  #endif

  #ifdef CMK_CUDA
    #define CMK_CUDA_PROGRESS_FREQ 50
    int cudaProgressCount = CMK_CUDA_PROGRESS_FREQ;
  #endif

  int isIdle=0;
  SCHEDULE_TOP
  while (1) {
    /* The interoperation will cost this little overhead in scheduling */
    if(CharmLibInterOperate) {
      if(CpvAccess(interopExitFlag)) {
        CpvAccess(interopExitFlag) = 0;
        break;
      }
    }
    #if CMK_OUT_OF_CORE
    int OOCHasTask = 0;
    if(OOCPrivateCheck != NULL){
      void * obj = OOCPrivateCheck();
      if(obj != NULL){
        OOCHasTask = 1;
        if (isIdle) {isIdle=0;CsdEndIdle();}
        OOCPrivateProcess(obj);
      }
    }
    if(OOCHasTask == 0)
    #endif  
    {
      msg = CsdNextMessage(&state);
      if (msg!=NULL) { /*A message is available-- process it*/
        if (isIdle) {isIdle=0;CsdEndIdle();}
        SCHEDULE_MESSAGE

        #if CMK_CELL
          if (progressCount <= 0) {
            /*OffloadAPIProgress();*/
            machine_OffloadAPIProgress();
            progressCount = CMK_CELL_PROGRESS_FREQ;
          }
          progressCount--;
        #endif

        #ifdef CMK_CUDA
          if (cudaProgressCount == 0) {
            gpuProgressFn(); 
            cudaProgressCount = CMK_CUDA_PROGRESS_FREQ; 
          }
          cudaProgressCount--; 
        #endif

      } else { /*No message available-- go (or remain) idle*/
        #if CMK_OUT_OF_CORE
        OOCHasTask = 0;
        if(OOCNodeCheck != NULL){
          void * obj = OOCNodeCheck();
          if(obj != NULL){
            OOCHasTask = 1;
            if (isIdle) {isIdle=0;CsdEndIdle();}
            OOCNodeProcess(obj);
          }
        }
        if(OOCHasTask == 0)
        #endif  
          SCHEDULE_IDLE

        #if CMK_CELL
          /*OffloadAPIProgress();*/
          machine_OffloadAPIProgress();
          progressCount = CMK_CELL_PROGRESS_FREQ;
        #endif

        #ifdef CMK_CUDA
          gpuProgressFn(); 
          cudaProgressCount = CMK_CUDA_PROGRESS_FREQ;
        #endif

      }
    }
    CsdPeriodic();
  }
}
int CsdScheduleCount(int maxmsgs)
{
  int isIdle=0;
  SCHEDULE_TOP
  while (1) {
    msg = CsdNextMessage(&state);
    if (msg!=NULL) { /*A message is available-- process it*/
      if (isIdle) {isIdle=0;CsdEndIdle();}
      maxmsgs--; 
      SCHEDULE_MESSAGE
      if (maxmsgs==0) break;
    } else { /*No message available-- go (or remain) idle*/
      SCHEDULE_IDLE
    }
    CsdPeriodic();
  }
  return maxmsgs;
}

void CsdSchedulePoll(void)
{
  SCHEDULE_TOP
  while (1)
  {
	CsdPeriodic();
        /*CmiMachineProgressImpl(); ??? */
	if (NULL!=(msg = CsdNextMessage(&state)))
	{
	     SCHEDULE_MESSAGE 
     	}
	else break;
  }
}

void CsdScheduleNodePoll(void)
{
  SCHEDULE_TOP
  while (1)
  {
	/*CsdPeriodic();*/
        /*CmiMachineProgressImpl(); ??? */
	if (NULL!=(msg = CsdNextLocalNodeMessage(&state)))
	{
	     SCHEDULE_MESSAGE 
     	}
	else break;
  }
}

void CmiDeliverSpecificMsg(handler)
int handler;
{
  int *msg; int side;
  void *localqueue = CpvAccess(CmiLocalQueue);
 
  side = 0;
  while (1) {
    CsdPeriodic();
    side ^= 1;
    if (side) msg = CmiGetNonLocal();
    else      msg = CdsFifo_Dequeue(localqueue);
    if (msg) {
      if (CmiGetHandler(msg)==handler) {
	CpvAccess(cQdState)->mProcessed++;
	CmiHandleMessage(msg);
	return;
      } else {
	CdsFifo_Enqueue(localqueue, msg);
      }
    }
  }
}
 
#endif /* CMK_CMIDELIVERS_USE_COMMON_CODE */

/***************************************************************************
 *
 * Standin Schedulers.
 *
 * We use the following strategy to make sure somebody's always running
 * the scheduler (CsdScheduler).  Initially, we assume the main thread
 * is responsible for this.  If the main thread blocks, we create a
 * "standin scheduler" thread to replace it.  If the standin scheduler
 * blocks, we create another standin scheduler to replace that one,
 * ad infinitum.  Collectively, the main thread and all the standin
 * schedulers are called "scheduling threads".
 *
 * Suppose the main thread is blocked waiting for data, and a standin
 * scheduler is running instead.  Suppose, then, that the data shows
 * up and the main thread is CthAwakened.  This causes a token to be
 * pushed into the queue.  When the standin pulls the token from the
 * queue and handles it, the standin goes to sleep, and control shifts
 * back to the main thread.  In this way, unnecessary standins are put
 * back to sleep.  These sleeping standins are stored on the
 * CthSleepingStandins list.
 *
 ***************************************************************************/

CpvStaticDeclare(CthThread, CthMainThread);
CpvStaticDeclare(CthThread, CthSchedulingThread);
CpvStaticDeclare(CthThread, CthSleepingStandins);
CpvDeclare(int      , CthResumeNormalThreadIdx);
CpvStaticDeclare(int      , CthResumeSchedulingThreadIdx);


void CthStandinCode()
{
  while (1) CsdScheduler(0);
}

/* this fix the function pointer for thread migration and pup */
static CthThread CthSuspendNormalThread()
{
  return CpvAccess(CthSchedulingThread);
}

void CthEnqueueSchedulingThread(CthThreadToken *token, int, int, unsigned int*);
CthThread CthSuspendSchedulingThread();

CthThread CthSuspendSchedulingThread()
{
  CthThread succ = CpvAccess(CthSleepingStandins);

  if (succ) {
    CpvAccess(CthSleepingStandins) = CthGetNext(succ);
  } else {
    succ = CthCreate(CthStandinCode, 0, 256000);
    CthSetStrategy(succ,
		   CthEnqueueSchedulingThread,
		   CthSuspendSchedulingThread);
  }
  
  CpvAccess(CthSchedulingThread) = succ;
  return succ;
}

/* Notice: For changes to the following function, make sure the function CthResumeNormalThreadDebug is also kept updated. */
void CthResumeNormalThread(CthThreadToken* token)
{
  CthThread t = token->thread;

  /* BIGSIM_OOC DEBUGGING
  CmiPrintf("Resume normal thread with token[%p] ==> thread[%p]\n", token, t);
  */

  if(t == NULL){
    free(token);
    return;
  }
#if CMK_TRACE_ENABLED
#if ! CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn))
    CthTraceResume(t);
/*    if(CpvAccess(_traceCoreOn)) 
	        resumeTraceCore();*/
#endif
#endif
  
  /* BIGSIM_OOC DEBUGGING
  CmiPrintf("In CthResumeNormalThread:   ");
  CthPrintThdMagic(t);
  */

  CthResume(t);
}

void CthResumeSchedulingThread(CthThreadToken  *token)
{
  CthThread t = token->thread;
  CthThread me = CthSelf();
  if (me == CpvAccess(CthMainThread)) {
    CthEnqueueSchedulingThread(CthGetToken(me),CQS_QUEUEING_FIFO, 0, 0);
  } else {
    CthSetNext(me, CpvAccess(CthSleepingStandins));
    CpvAccess(CthSleepingStandins) = me;
  }
  CpvAccess(CthSchedulingThread) = t;
#if CMK_TRACE_ENABLED
#if ! CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn))
    CthTraceResume(t);
/*    if(CpvAccess(_traceCoreOn)) 
	        resumeTraceCore();*/
#endif
#endif
  CthResume(t);
}

void CthEnqueueNormalThread(CthThreadToken* token, int s, 
				   int pb,unsigned int *prio)
{
  CmiSetHandler(token, CpvAccess(CthResumeNormalThreadIdx));
  CsdEnqueueGeneral(token, s, pb, prio);
}

void CthEnqueueSchedulingThread(CthThreadToken* token, int s, 
				       int pb,unsigned int *prio)
{
  CmiSetHandler(token, CpvAccess(CthResumeSchedulingThreadIdx));
  CsdEnqueueGeneral(token, s, pb, prio);
}

void CthSetStrategyDefault(CthThread t)
{
  CthSetStrategy(t,
		 CthEnqueueNormalThread,
		 CthSuspendNormalThread);
}

void CthSchedInit()
{
  CpvInitialize(CthThread, CthMainThread);
  CpvInitialize(CthThread, CthSchedulingThread);
  CpvInitialize(CthThread, CthSleepingStandins);
  CpvInitialize(int      , CthResumeNormalThreadIdx);
  CpvInitialize(int      , CthResumeSchedulingThreadIdx);

  CpvAccess(CthMainThread) = CthSelf();
  CpvAccess(CthSchedulingThread) = CthSelf();
  CpvAccess(CthSleepingStandins) = 0;
  CpvAccess(CthResumeNormalThreadIdx) =
    CmiRegisterHandler((CmiHandler)CthResumeNormalThread);
  CpvAccess(CthResumeSchedulingThreadIdx) =
    CmiRegisterHandler((CmiHandler)CthResumeSchedulingThread);
  CthSetStrategy(CthSelf(),
		 CthEnqueueSchedulingThread,
		 CthSuspendSchedulingThread);
}

void CsdInit(argv)
  char **argv;
{
  CpvInitialize(void *, CsdSchedQueue);
  CpvInitialize(int,   CsdStopFlag);
  CpvInitialize(int,   CsdLocalCounter);
  int argCsdLocalMax=CSD_LOCAL_MAX_DEFAULT;
  int argmaxset = CmiGetArgIntDesc(argv,"+csdLocalMax",&argCsdLocalMax,"Set the max number of local messages to process before forcing a check for remote messages.");
  if (CmiMyRank() == 0 ) CsdLocalMax = argCsdLocalMax;
  CpvAccess(CsdLocalCounter) = argCsdLocalMax;
  CpvAccess(CsdSchedQueue) = (void *)CqsCreate();
   #if CMK_USE_STL_MSGQ
   if (CmiMyPe() == 0) CmiPrintf("Charm++> Using STL-based msgQ:\n");
   #endif
   #if CMK_RANDOMIZED_MSGQ
   if (CmiMyPe() == 0) CmiPrintf("Charm++> Using randomized msgQ. Priorities will not be respected!\n");
   #endif

#if CMK_OBJECT_QUEUE_AVAILABLE
  CpvInitialize(void *,CsdObjQueue);
  CpvAccess(CsdObjQueue) = CdsFifo_Create();
#endif

#if CMK_NODE_QUEUE_AVAILABLE
  CsvInitialize(CmiLock, CsdNodeQueueLock);
  CsvInitialize(void *, CsdNodeQueue);
  if (CmiMyRank() ==0) {
	CsvAccess(CsdNodeQueueLock) = CmiCreateLock();
	CsvAccess(CsdNodeQueue) = (void *)CqsCreate();
  }
  CmiNodeAllBarrier();
#endif

#if CMK_GRID_QUEUE_AVAILABLE
  CsvInitialize(void *, CsdGridQueue);
  CpvAccess(CsdGridQueue) = (void *)CqsCreate();
#endif

  CpvAccess(CsdStopFlag)  = 0;
}



/** 
 *  @}
 */


/*****************************************************************************
 *
 * Vector Send
 *
 * The last parameter "system" is by default at zero, in which case the normal
 * messages are sent. If it is set to 1, the CmiChunkHeader prepended to every
 * CmiAllocced message will also be sent (except for the first one). Useful for
 * AllToAll communication, and other system features. If system is 1, also all
 * the messages will be padded to 8 bytes. Thus, the caller must be aware of
 * that.
 *
 ****************************************************************************/

#if CMK_VECTOR_SEND_USES_COMMON_CODE

void CmiSyncVectorSend(int destPE, int n, int *sizes, char **msgs) {
  int total;
  char *mesg;
#if CMK_USE_IBVERBS
  VECTOR_COMPACT(total, mesg, n, sizes, msgs,sizeof(infiCmiChunkHeader));
#else
  VECTOR_COMPACT(total, mesg, n, sizes, msgs,sizeof(CmiChunkHeader));
#endif	
  CmiSyncSendAndFree(destPE, total, mesg);
}

CmiCommHandle CmiASyncVectorSend(int destPE, int n, int *sizes, char **msgs) {
  CmiSyncVectorSend(destPE, n, sizes, msgs);
  return NULL;
}

void CmiSyncVectorSendAndFree(int destPE, int n, int *sizes, char **msgs) {
  int i;
  CmiSyncVectorSend(destPE, n, sizes, msgs);
  for(i=0;i<n;i++) CmiFree(msgs[i]);
  CmiFree(sizes);
  CmiFree(msgs);
}

#endif

/*****************************************************************************
 *
 * Reduction management
 *
 * Only one reduction can be active at a single time in the program.
 * Moreover, since every call is supposed to pass in the same arguments,
 * having some static variables is not a problem for multithreading.
 * 
 * Except for "data" and "size", all the other parameters (which are all function
 * pointers) MUST be the same in every processor. Having different processors
 * pass in different function pointers results in an undefined behaviour.
 * 
 * The data passed in to CmiReduce and CmiNodeReduce is deleted by the system,
 * and MUST be allocated with CmiAlloc. The data passed in to the "Struct"
 * functions is deleted with the provided function, or it is left intact if no
 * function is specified.
 * 
 * The destination handler for the the first form MUST be embedded into the
 * message's header.
 * 
 * The pup function is used to pup the input data structure into a message to
 * be sent to the parent processor. This pup routine is currently used only
 * for sizing and packing, NOT unpacking. It MUST be non-null.
 * 
 * The merge function receives as first parameter the input "data", being it
 * a message or a complex data structure (it is up to the user to interpret it
 * correctly), and a list of incoming (packed) messages from the children.
 * The merge function is responsible to delete "data" if this is no longer needed.
 * The system will be in charge of deleting the messages passed in as the second
 * argument, and the return value of the function (using the provided deleteFn in
 * the second version, or CmiFree in the first). The merge function can return
 * data if the merge can be performed in-place. It MUST be non-null.
 * 
 * At the destination, on processor zero, the final data returned by the last
 * merge call will not be deleted by the system, and the CmiHandler function
 * will be in charge of its deletion.
 * 
 * CmiReduce/CmiReduceStruct MUST be called once by every processor,
 * CmiNodeReduce/CmiNodeReduceStruct MUST be called once by every node, and in
 * particular by the rank zero in each node.
 ****************************************************************************/

CpvStaticDeclare(int, CmiReductionMessageHandler);
CpvStaticDeclare(int, CmiReductionDynamicRequestHandler);

CpvStaticDeclare(CmiReduction**, _reduce_info);
CpvStaticDeclare(int, _reduce_info_size); /* This is the log2 of the size of the array */
CpvStaticDeclare(CmiUInt2, _reduce_seqID_global); /* This is used only by global reductions */
CpvStaticDeclare(CmiUInt2, _reduce_seqID_request);
CpvStaticDeclare(CmiUInt2, _reduce_seqID_dynamic);

enum {
  CmiReductionID_globalOffset = 0, /* Reductions that involve the whole set of processors */
  CmiReductionID_requestOffset = 1, /* Reductions IDs that are requested by all the processors (i.e during intialization) */
  CmiReductionID_dynamicOffset = 2, /* Reductions IDs that are requested by only one processor (typically at runtime) */
  CmiReductionID_multiplier = 3
};

CmiReduction* CmiGetReductionCreate(int id, short int numChildren) {
  int index = id & ~((~0u)<<CpvAccess(_reduce_info_size));
  CmiReduction *red = CpvAccess(_reduce_info)[index];
  if (red != NULL && red->seqID != id) {
    /* The table needs to be expanded */
    CmiAbort("Too many simultaneous reductions");
  }
  if (red == NULL || red->numChildren < numChildren) {
    CmiReduction *newred;
    CmiAssert(red == NULL || red->localContributed == 0);
    if (numChildren == 0) numChildren = 4;
    newred = (CmiReduction*)malloc(sizeof(CmiReduction)+numChildren*sizeof(void*));
    newred->numRemoteReceived = 0;
    newred->localContributed = 0;
    newred->seqID = id;
    if (red != NULL) {
      memcpy(newred, red, sizeof(CmiReduction)+red->numChildren*sizeof(void*));
      free(red);
    }
    red = newred;
    red->numChildren = numChildren;
    red->remoteData = (char**)(red+1);
    CpvAccess(_reduce_info)[index] = red;
  }
  return red;
}

CmiReduction* CmiGetReduction(int id) {
  return CmiGetReductionCreate(id, 0);
}

void CmiClearReduction(int id) {
  int index = id & ~((~0u)<<CpvAccess(_reduce_info_size));
  free(CpvAccess(_reduce_info)[index]);
  CpvAccess(_reduce_info)[index] = NULL;
}

CmiReduction* CmiGetNextReduction(short int numChildren) {
  int id = CpvAccess(_reduce_seqID_global);
  CpvAccess(_reduce_seqID_global) += CmiReductionID_multiplier;
  if (id > 0xFFF0) CpvAccess(_reduce_seqID_global) = CmiReductionID_globalOffset;
  return CmiGetReductionCreate(id, numChildren);
}

CmiReductionID CmiGetGlobalReduction() {
  return CpvAccess(_reduce_seqID_request)+=CmiReductionID_multiplier;
}

CmiReductionID CmiGetDynamicReduction() {
  if (CmiMyPe() != 0) CmiAbort("Cannot call CmiGetDynamicReduction on processors other than zero!\n");
  return CpvAccess(_reduce_seqID_dynamic)+=CmiReductionID_multiplier;
}

void CmiReductionHandleDynamicRequest(char *msg) {
  int *values = (int*)(msg+CmiMsgHeaderSizeBytes);
  int pe = values[0];
  int size = CmiMsgHeaderSizeBytes+2*sizeof(int)+values[1];
  values[0] = CmiGetDynamicReduction();
  CmiSetHandler(msg, CmiGetXHandler(msg));
  if (pe >= 0) {
    CmiSyncSendAndFree(pe, size, msg);
  } else {
    CmiSyncBroadcastAllAndFree(size, msg);
  }
}

void CmiGetDynamicReductionRemote(int handlerIdx, int pe, int dataSize, void *data) {
  int size = CmiMsgHeaderSizeBytes+2*sizeof(int)+dataSize;
  char *msg = (char*)CmiAlloc(size);
  int *values = (int*)(msg+CmiMsgHeaderSizeBytes);
  values[0] = pe;
  values[1] = dataSize;
  CmiSetXHandler(msg, handlerIdx);
  if (dataSize) memcpy(msg+CmiMsgHeaderSizeBytes+2*sizeof(int), data, dataSize);
  if (CmiMyPe() == 0) {
    CmiReductionHandleDynamicRequest(msg);
  } else {
    /* send the request to processor 0 */
    CmiSetHandler(msg, CpvAccess(CmiReductionDynamicRequestHandler));
    CmiSyncSendAndFree(0, size, msg);
  }
}

void CmiSendReduce(CmiReduction *red) {
  void *mergedData, *msg;
  int msg_size;
  if (!red->localContributed || red->numChildren != red->numRemoteReceived) return;
  mergedData = red->localData;
  msg_size = red->localSize;
  if (red->numChildren > 0) {
    int i, offset=0;
    if (red->ops.pupFn != NULL) {
      offset = CmiReservedHeaderSize;
      for (i=0; i<red->numChildren; ++i) red->remoteData[i] += offset;
    }
    mergedData = (red->ops.mergeFn)(&msg_size, red->localData, (void **)red->remoteData, red->numChildren);
    for (i=0; i<red->numChildren; ++i) CmiFree(red->remoteData[i] - offset);
  }
  /*CpvAccess(_reduce_num_children) = 0;*/
  /*CpvAccess(_reduce_received) = 0;*/
  msg = mergedData;
  if (red->parent != -1) {
    if (red->ops.pupFn != NULL) {
      pup_er p = pup_new_sizer();
      (red->ops.pupFn)(p, mergedData);
      msg_size = pup_size(p) + CmiReservedHeaderSize;
      pup_destroy(p);
      msg = CmiAlloc(msg_size);
      p = pup_new_toMem((void*)(((char*)msg)+CmiReservedHeaderSize));
      (red->ops.pupFn)(p, mergedData);
      pup_destroy(p);
      if (red->ops.deleteFn != NULL) (red->ops.deleteFn)(red->localData);
    }
    CmiSetHandler(msg, CpvAccess(CmiReductionMessageHandler));
    CmiSetRedID(msg, red->seqID);
    /*CmiPrintf("CmiSendReduce(%d): sending %d bytes to %d\n",CmiMyPe(),msg_size,red->parent);*/
    CmiSyncSendAndFree(red->parent, msg_size, msg);
  } else {
    (red->ops.destination)(msg);
  }
  CmiClearReduction(red->seqID);
}

void *CmiReduceMergeFn_random(int *size, void *data, void** remote, int n) {
  return data;
}

void CmiResetGlobalReduceSeqID(){
	CpvAccess(_reduce_seqID_global) = 0;
}

static void CmiGlobalReduce(void *msg, int size, CmiReduceMergeFn mergeFn, CmiReduction *red) {
  CmiAssert(red->localContributed == 0);
  red->localContributed = 1;
  red->localData = msg;
  red->localSize = size;
  red->numChildren = CmiNumSpanTreeChildren(CmiMyPe());
  red->parent = CmiSpanTreeParent(CmiMyPe());
  red->ops.destination = (CmiHandler)CmiGetHandlerFunction(msg);
  red->ops.mergeFn = mergeFn;
  red->ops.pupFn = NULL;
  /*CmiPrintf("[%d] CmiReduce::local %hd parent=%d, numChildren=%d\n",CmiMyPe(),red->seqID,red->parent,red->numChildren);*/
  CmiSendReduce(red);
}

static void CmiGlobalReduceStruct(void *data, CmiReducePupFn pupFn,
                     CmiReduceMergeFn mergeFn, CmiHandler dest,
                     CmiReduceDeleteFn deleteFn, CmiReduction *red) {
  CmiAssert(red->localContributed == 0);
  red->localContributed = 1;
  red->localData = data;
  red->localSize = 0;
  red->numChildren = CmiNumSpanTreeChildren(CmiMyPe());
  red->parent = CmiSpanTreeParent(CmiMyPe());
  red->ops.destination = dest;
  red->ops.mergeFn = mergeFn;
  red->ops.pupFn = pupFn;
  red->ops.deleteFn = deleteFn;
  /*CmiPrintf("[%d] CmiReduceStruct::local %hd parent=%d, numChildren=%d\n",CmiMyPe(),red->seqID,red->parent,red->numChildren);*/
  CmiSendReduce(red);
}

void CmiReduce(void *msg, int size, CmiReduceMergeFn mergeFn) {
  CmiReduction *red = CmiGetNextReduction(CmiNumSpanTreeChildren(CmiMyPe()));
  CmiGlobalReduce(msg, size, mergeFn, red);
}

void CmiReduceStruct(void *data, CmiReducePupFn pupFn,
                     CmiReduceMergeFn mergeFn, CmiHandler dest,
                     CmiReduceDeleteFn deleteFn) {
  CmiReduction *red = CmiGetNextReduction(CmiNumSpanTreeChildren(CmiMyPe()));
  CmiGlobalReduceStruct(data, pupFn, mergeFn, dest, deleteFn, red);
}

void CmiReduceID(void *msg, int size, CmiReduceMergeFn mergeFn, CmiReductionID id) {
  CmiReduction *red = CmiGetReductionCreate(id, CmiNumSpanTreeChildren(CmiMyPe()));
  CmiGlobalReduce(msg, size, mergeFn, red);
}

void CmiReduceStructID(void *data, CmiReducePupFn pupFn,
                     CmiReduceMergeFn mergeFn, CmiHandler dest,
                     CmiReduceDeleteFn deleteFn, CmiReductionID id) {
  CmiReduction *red = CmiGetReductionCreate(id, CmiNumSpanTreeChildren(CmiMyPe()));
  CmiGlobalReduceStruct(data, pupFn, mergeFn, dest, deleteFn, red);
}

void CmiListReduce(int npes, int *pes, void *msg, int size, CmiReduceMergeFn mergeFn, CmiReductionID id) {
  CmiReduction *red = CmiGetReductionCreate(id, CmiNumSpanTreeChildren(CmiMyPe()));
  int myPos;
  CmiAssert(red->localContributed == 0);
  red->localContributed = 1;
  red->localData = msg;
  red->localSize = size;
  for (myPos=0; myPos<npes; ++myPos) {
    if (pes[myPos] == CmiMyPe()) break;
  }
  CmiAssert(myPos < npes);
  red->numChildren = npes - (myPos << 2) - 1;
  if (red->numChildren > 4) red->numChildren = 4;
  if (red->numChildren < 0) red->numChildren = 0;
  if (myPos == 0) red->parent = -1;
  else red->parent = pes[(myPos - 1) >> 2];
  red->ops.destination = (CmiHandler)CmiGetHandlerFunction(msg);
  red->ops.mergeFn = mergeFn;
  red->ops.pupFn = NULL;
  /*CmiPrintf("[%d] CmiListReduce::local %hd parent=%d, numChildren=%d\n",CmiMyPe(),red->seqID,red->parent,red->numChildren);*/
  CmiSendReduce(red);
}

void CmiListReduceStruct(int npes, int *pes,
                     void *data, CmiReducePupFn pupFn,
                     CmiReduceMergeFn mergeFn, CmiHandler dest,
                     CmiReduceDeleteFn deleteFn, CmiReductionID id) {
  CmiReduction *red = CmiGetReductionCreate(id, CmiNumSpanTreeChildren(CmiMyPe()));
  int myPos;
  CmiAssert(red->localContributed == 0);
  red->localContributed = 1;
  red->localData = data;
  red->localSize = 0;
  for (myPos=0; myPos<npes; ++myPos) {
    if (pes[myPos] == CmiMyPe()) break;
  }
  CmiAssert(myPos < npes);
  red->numChildren = npes - (myPos << 2) - 1;
  if (red->numChildren > 4) red->numChildren = 4;
  if (red->numChildren < 0) red->numChildren = 0;
  red->parent = (myPos - 1) >> 2;
  if (myPos == 0) red->parent = -1;
  red->ops.destination = dest;
  red->ops.mergeFn = mergeFn;
  red->ops.pupFn = pupFn;
  red->ops.deleteFn = deleteFn;
  CmiSendReduce(red);
}

void CmiGroupReduce(CmiGroup grp, void *msg, int size, CmiReduceMergeFn mergeFn, CmiReductionID id) {
  int npes, *pes;
  CmiLookupGroup(grp, &npes, &pes);
  CmiListReduce(npes, pes, msg, size, mergeFn, id);
}

void CmiGroupReduceStruct(CmiGroup grp, void *data, CmiReducePupFn pupFn,
                     CmiReduceMergeFn mergeFn, CmiHandler dest,
                     CmiReduceDeleteFn deleteFn, CmiReductionID id) {
  int npes, *pes;
  CmiLookupGroup(grp, &npes, &pes);
  CmiListReduceStruct(npes, pes, data, pupFn, mergeFn, dest, deleteFn, id);
}

void CmiNodeReduce(void *data, int size, CmiReduceMergeFn mergeFn, int redID, int numChildren, int parent) {
  CmiAbort("Feel free to implement CmiNodeReduce...");
  /*
  CmiAssert(CmiRankOf(CmiMyPe()) == 0);
  CpvAccess(_reduce_data) = data;
  CpvAccess(_reduce_data_size) = size;
  CpvAccess(_reduce_parent) = CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode()));
  _reduce_destination = (CmiHandler)CmiGetHandlerFunction(data);
  _reduce_pupFn = NULL;
  _reduce_mergeFn = mergeFn;
  CpvAccess(_reduce_num_children) = CmiNumNodeSpanTreeChildren(CmiMyNode());
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce(size);
  */
}
#if 0
void CmiNodeReduce(void *data, int size, void * (*mergeFn)(void*,void**,int), int redID) {
  CmiNodeReduce(data, size, mergeFn, redID, CmiNumNodeSpanTreeChildren(CmiMyNode()),
      CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode())));
}
void CmiNodeReduce(void *data, int size, void * (*mergeFn)(void*,void**,int), int numChildren, int parent) {
  CmiNodeReduce(data, size, mergeFn, CmiReduceNextID(), numChildren, parent);
}
void CmiNodeReduce(void *data, int size, void * (*mergeFn)(void*,void**,int)) {
  CmiNodeReduce(data, size, mergeFn, CmiReduceNextID(), CmiNumNodeSpanTreeChildren(CmiMyNode()),
      CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode())));
}
#endif

void CmiNodeReduceStruct(void *data, CmiReducePupFn pupFn,
                         CmiReduceMergeFn mergeFn, CmiHandler dest,
                         CmiReduceDeleteFn deleteFn) {
  CmiAbort("Feel free to implement CmiNodeReduceStruct...");
/*
  CmiAssert(CmiRankOf(CmiMyPe()) == 0);
  CpvAccess(_reduce_data) = data;
  CpvAccess(_reduce_parent) = CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode()));
  _reduce_destination = dest;
  _reduce_pupFn = pupFn;
  _reduce_mergeFn = mergeFn;
  _reduce_deleteFn = deleteFn;
  CpvAccess(_reduce_num_children) = CmiNumNodeSpanTreeChildren(CmiMyNode());
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce(0);
  */
}

void CmiHandleReductionMessage(void *msg) {
  CmiReduction *red = CmiGetReduction(CmiGetRedID(msg));
  if (red->numRemoteReceived == red->numChildren) red = CmiGetReductionCreate(CmiGetRedID(msg), red->numChildren+4);
  red->remoteData[red->numRemoteReceived++] = msg;
  /*CmiPrintf("[%d] CmiReduce::remote %hd\n",CmiMyPe(),red->seqID);*/
  CmiSendReduce(red);
/*
  CpvAccess(_reduce_msg_list)[CpvAccess(_reduce_received)++] = msg;
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce();
  / *else CmiPrintf("CmiHandleReductionMessage(%d): %d - %d\n",CmiMyPe(),CpvAccess(_reduce_received),CpvAccess(_reduce_num_children));*/
}

void CmiReductionsInit() {
  int i;
  CpvInitialize(int, CmiReductionMessageHandler);
  CpvAccess(CmiReductionMessageHandler) = CmiRegisterHandler((CmiHandler)CmiHandleReductionMessage);
  CpvInitialize(int, CmiReductionDynamicRequestHandler);
  CpvAccess(CmiReductionDynamicRequestHandler) = CmiRegisterHandler((CmiHandler)CmiReductionHandleDynamicRequest);
  CpvInitialize(CmiUInt2, _reduce_seqID_global);
  CpvAccess(_reduce_seqID_global) = CmiReductionID_globalOffset;
  CpvInitialize(CmiUInt2, _reduce_seqID_request);
  CpvAccess(_reduce_seqID_request) = CmiReductionID_requestOffset;
  CpvInitialize(CmiUInt2, _reduce_seqID_dynamic);
  CpvAccess(_reduce_seqID_dynamic) = CmiReductionID_dynamicOffset;
  CpvInitialize(int, _reduce_info_size);
  CpvAccess(_reduce_info_size) = 4;
  CpvInitialize(CmiReduction**, _reduce_info);
  CpvAccess(_reduce_info) = malloc(16*sizeof(CmiReduction*));
  for (i=0; i<16; ++i) CpvAccess(_reduce_info)[i] = NULL;
}

/*****************************************************************************
 *
 * Multicast groups
 *
 ****************************************************************************/

#if CMK_MULTICAST_DEF_USE_COMMON_CODE

typedef struct GroupDef
{
  union {
    char core[CmiMsgHeaderSizeBytes];
    struct GroupDef *next;
  } core;
  CmiGroup group;
  int npes;
  int pes[1];
}
*GroupDef;

#define GROUPTAB_SIZE 101

CpvStaticDeclare(int, CmiGroupHandlerIndex);
CpvStaticDeclare(int, CmiGroupCounter);
CpvStaticDeclare(GroupDef *, CmiGroupTable);

void CmiGroupHandler(GroupDef def)
{
  /* receive group definition, insert into group table */
  GroupDef *table = CpvAccess(CmiGroupTable);
  unsigned int hashval, bucket;
  hashval = (def->group.id ^ def->group.pe);
  bucket = hashval % GROUPTAB_SIZE;
  def->core.next = table[bucket];
  table[bucket] = def;
}

CmiGroup CmiEstablishGroup(int npes, int *pes)
{
  /* build new group definition, broadcast it */
  CmiGroup grp; GroupDef def; int len, i;
  grp.id = CpvAccess(CmiGroupCounter)++;
  grp.pe = CmiMyPe();
  len = sizeof(struct GroupDef)+(npes*sizeof(int));
  def = (GroupDef)CmiAlloc(len);
  def->group = grp;
  def->npes = npes;
  for (i=0; i<npes; i++)
    def->pes[i] = pes[i];
  CmiSetHandler(def, CpvAccess(CmiGroupHandlerIndex));
  CmiSyncBroadcastAllAndFree(len, def);
  return grp;
}

void CmiLookupGroup(CmiGroup grp, int *npes, int **pes)
{
  unsigned int hashval, bucket;  GroupDef def;
  GroupDef *table = CpvAccess(CmiGroupTable);
  hashval = (grp.id ^ grp.pe);
  bucket = hashval % GROUPTAB_SIZE;
  for (def=table[bucket]; def; def=def->core.next) {
    if ((def->group.id == grp.id)&&(def->group.pe == grp.pe)) {
      *npes = def->npes;
      *pes = def->pes;
      return;
    }
  }
  *npes = 0; *pes = 0;
}

void CmiGroupInit()
{
  CpvInitialize(int, CmiGroupHandlerIndex);
  CpvInitialize(int, CmiGroupCounter);
  CpvInitialize(GroupDef *, CmiGroupTable);
  CpvAccess(CmiGroupHandlerIndex) = CmiRegisterHandler((CmiHandler)CmiGroupHandler);
  CpvAccess(CmiGroupCounter) = 0;
  CpvAccess(CmiGroupTable) =
    (GroupDef*)calloc(GROUPTAB_SIZE, sizeof(GroupDef));
  if (CpvAccess(CmiGroupTable) == 0)
    CmiAbort("Memory Allocation Error");
}

#endif

/*****************************************************************************
 *
 * Common List-Cast and Multicast Code
 *
 ****************************************************************************/

#if CMK_MULTICAST_LIST_USE_COMMON_CODE

void CmiSyncListSendFn(int npes, int *pes, int len, char *msg)
{
  int i;
#if CMK_BROADCAST_USE_CMIREFERENCE
  for(i=0;i<npes;i++) {
    if (pes[i] == CmiMyPe())
      CmiSyncSend(pes[i], len, msg);
    else {
      CmiReference(msg);
      CmiSyncSendAndFree(pes[i], len, msg);
    }
  }
#else
  for(i=0;i<npes;i++) {
    CmiSyncSend(pes[i], len, msg);
  }
#endif
}

CmiCommHandle CmiAsyncListSendFn(int npes, int *pes, int len, char *msg)
{
  /* A better asynchronous implementation may be wanted, but at least it works */
  CmiSyncListSendFn(npes, pes, len, msg);
  return (CmiCommHandle) 0;
}

void CmiFreeListSendFn(int npes, int *pes, int len, char *msg)
{
#if CMK_BROADCAST_USE_CMIREFERENCE
  if (npes == 1) {
    CmiSyncSendAndFree(pes[0], len, msg);
    return;
  }
  CmiSyncListSendFn(npes, pes, len, msg);
  CmiFree(msg);
#else
  int i;
  for(i=0;i<npes-1;i++) {
    CmiSyncSend(pes[i], len, msg);
  }
  if (npes>0)
    CmiSyncSendAndFree(pes[npes-1], len, msg);
  else 
    CmiFree(msg);
#endif
}

#endif

#if CMK_MULTICAST_GROUP_USE_COMMON_CODE

typedef struct MultiMsg
{
  char core[CmiMsgHeaderSizeBytes];
  CmiGroup group;
  int pos;
  int origlen;
}
*MultiMsg;


CpvDeclare(int, CmiMulticastHandlerIndex);

void CmiMulticastDeliver(MultiMsg msg)
{
  int npes, *pes; int olen, nlen, pos, child1, child2;
  olen = msg->origlen;
  nlen = olen + sizeof(struct MultiMsg);
  CmiLookupGroup(msg->group, &npes, &pes);
  if (pes==0) {
    CmiSyncSendAndFree(CmiMyPe(), nlen, msg);
    return;
  }
  if (npes==0) {
    CmiFree(msg);
    return;
  }
  if (msg->pos == -1) {
    msg->pos=0;
    CmiSyncSendAndFree(pes[0], nlen, msg);
    return;
  }
  pos = msg->pos;
  child1 = ((pos+1)<<1);
  child2 = child1-1;
  if (child1 < npes) {
    msg->pos = child1;
    CmiSyncSend(pes[child1], nlen, msg);
  }
  if (child2 < npes) {
    msg->pos = child2;
    CmiSyncSend(pes[child2], nlen, msg);
  }
  if(olen < sizeof(struct MultiMsg)) {
    memcpy(msg, msg+1, olen);
  } else {
    memcpy(msg, (((char*)msg)+olen), sizeof(struct MultiMsg));
  }
  CmiSyncSendAndFree(CmiMyPe(), olen, msg);
}

void CmiMulticastHandler(MultiMsg msg)
{
  CmiMulticastDeliver(msg);
}

void CmiSyncMulticastFn(CmiGroup grp, int len, char *msg)
{
  int newlen; MultiMsg newmsg;
  newlen = len + sizeof(struct MultiMsg);
  newmsg = (MultiMsg)CmiAlloc(newlen);
  if(len < sizeof(struct MultiMsg)) {
    memcpy(newmsg+1, msg, len);
  } else {
    memcpy(newmsg+1, msg+sizeof(struct MultiMsg), len-sizeof(struct MultiMsg));
    memcpy(((char *)newmsg+len), msg, sizeof(struct MultiMsg));
  }
  newmsg->group = grp;
  newmsg->origlen = len;
  newmsg->pos = -1;
  CmiSetHandler(newmsg, CpvAccess(CmiMulticastHandlerIndex));
  CmiMulticastDeliver(newmsg);
}

void CmiFreeMulticastFn(CmiGroup grp, int len, char *msg)
{
  CmiSyncMulticastFn(grp, len, msg);
  CmiFree(msg);
}

CmiCommHandle CmiAsyncMulticastFn(CmiGroup grp, int len, char *msg)
{
  CmiError("Async Multicast not implemented.");
  return (CmiCommHandle) 0;
}

void CmiMulticastInit()
{
  CpvInitialize(int, CmiMulticastHandlerIndex);
  CpvAccess(CmiMulticastHandlerIndex) =
    CmiRegisterHandler((CmiHandler)CmiMulticastHandler);
}

#endif

#if CONVERSE_VERSION_SHMEM && CMK_ARENA_MALLOC
extern void *arena_malloc(int size);
extern void arena_free(void *blockPtr);
#endif

/***************************************************************************
 *
 * Memory Allocation routines 
 *
 * A block of memory can consist of multiple chunks.  Each chunk has
 * a sizefield and a refcount.  The first chunk's refcount is a reference
 * count.  That's how many CmiFrees it takes to free the message.
 * Subsequent chunks have a refcount which is less than zero.  This is
 * the offset back to the start of the first chunk.
 *
 * Each chunk has a CmiChunkHeader before the user data, with the fields:
 *
 *  size: The user-allocated size of the chunk, in bytes.
 *
 *  ref: A magic reference count object. Ordinary blocks start with
 *     reference count 1.  When the reference count reaches zero,
 *     the block is deleted.  To support nested buffers, the 
 *     reference count can also be negative, which means it is 
 *     a byte offset to the enclosing buffer's reference count.
 *
 ***************************************************************************/


void *CmiAlloc(int size)
{

  char *res;

#if CONVERSE_VERSION_SHMEM && CMK_ARENA_MALLOC
  res = (char*) arena_malloc(size+sizeof(CmiChunkHeader));
#elif CMK_USE_IBVERBS | CMK_USE_IBUD
  res = (char *) infi_CmiAlloc(size+sizeof(CmiChunkHeader));
#elif CMK_CONVERSE_UGNI
  res =(char *) LrtsAlloc(size, sizeof(CmiChunkHeader));
#elif CONVERSE_POOL
  res =(char *) CmiPoolAlloc(size+sizeof(CmiChunkHeader));
#elif USE_MPI_CTRLMSG_SCHEME && CMK_CONVERSE_MPI
  MPI_Alloc_mem(size+sizeof(CmiChunkHeader), MPI_INFO_NULL, &res);
#elif CMK_SMP && CMK_BLUEGENEQ && SPECIFIC_PCQUEUE
  res = (char *) CmiAlloc_bgq(size+sizeof(CmiChunkHeader));
#elif CMK_SMP && CMK_PPC_ATOMIC_QUEUE
  res = (char *) CmiAlloc_ppcq(size+sizeof(CmiChunkHeader));
#else
  res =(char *) malloc_nomigrate(size+sizeof(CmiChunkHeader));
#endif

  _MEMCHECK(res);

#ifdef MEMMONITOR
  CpvAccess(MemoryUsage) += size+sizeof(CmiChunkHeader);
  CpvAccess(AllocCount)++;
  CpvAccess(BlocksAllocated)++;
  if (CpvAccess(MemoryUsage) > CpvAccess(HiWaterMark)) {
    CpvAccess(HiWaterMark) = CpvAccess(MemoryUsage);
  }
  if (CpvAccess(MemoryUsage) > 1.1 * CpvAccess(ReportedHiWaterMark)) {
    CmiPrintf("HIMEM STAT PE%d: %d Allocs, %d blocks, %lu K, Max %lu K\n",
	    CmiMyPe(), CpvAccess(AllocCount), CpvAccess(BlocksAllocated),
            CpvAccess(MemoryUsage)/1024, CpvAccess(HiWaterMark)/1024);
    CpvAccess(ReportedHiWaterMark) = CpvAccess(MemoryUsage);
  }
  if ((CpvAccess(AllocCount) % 1000) == 0) {
    CmiPrintf("MEM STAT PE%d: %d Allocs, %d blocks, %lu K, Max %lu K\n",
	    CmiMyPe(), CpvAccess(AllocCount), CpvAccess(BlocksAllocated),
            CpvAccess(MemoryUsage)/1024, CpvAccess(HiWaterMark)/1024);
  }
#endif

  res+=sizeof(CmiChunkHeader);
  CmiAssert((intptr_t)res % ALIGN_BYTES == 0);

  SIZEFIELD(res)=size;
  REFFIELD(res)=1;
  return (void *)res;
}

/** Follow the header links out to the most enclosing block */
static void *CmiAllocFindEnclosing(void *blk) {
  int refCount = REFFIELD(blk);
  while (refCount < 0) {
    blk = (void *)((char*)blk+refCount); /* Jump to enclosing block */
    refCount = REFFIELD(blk);
  }
  return blk;
}

int CmiGetReference(void *blk)
{
  return REFFIELD(CmiAllocFindEnclosing(blk));
}

/** Increment the reference count for this block's owner.
    This call must be matched by an equivalent CmiFree. */
void CmiReference(void *blk)
{
  REFFIELD(CmiAllocFindEnclosing(blk))++;
}

/** Return the size of the user portion of this block. */
int CmiSize(void *blk)
{
  return SIZEFIELD(blk);
}

/** Decrement the reference count for this block. */
void CmiFree(void *blk)
{
  void *parentBlk=CmiAllocFindEnclosing(blk);
  int refCount=REFFIELD(parentBlk);
#if CMK_ERROR_CHECKING
  if(refCount==0) /* Logic error: reference count shouldn't already have been zero */
    CmiAbort("CmiFree reference count was zero-- is this a duplicate free?");
#endif
  refCount--;
  REFFIELD(parentBlk) = refCount;
  if(refCount==0) { /* This was the last reference to the block-- free it */
#ifdef MEMMONITOR
    int size=SIZEFIELD(parentBlk);
    if (size > 1000000000) /* Absurdly large size field-- warning */
      CmiPrintf("MEMSTAT Uh-oh -- SIZEFIELD=%d\n",size);
    CpvAccess(MemoryUsage) -= (size + sizeof(CmiChunkHeader));
    CpvAccess(BlocksAllocated)--;
#endif

#if CONVERSE_VERSION_SHMEM && CMK_ARENA_MALLOC
    arena_free(BLKSTART(parentBlk));
#elif CMK_USE_IBVERBS | CMK_USE_IBUD
    /* is this message the head of a MultipleSend that we received?
       Then the parts with INFIMULTIPOOL have metadata which must be 
       unregistered and freed.  */
#ifdef CMK_IBVERS_CLEAN_MULTIPLESEND
    if(CmiGetHandler(parentBlk)==CpvAccess(CmiMainHandlerIDP))
      {
	infi_freeMultipleSend(parentBlk);
      }
#endif
    infi_CmiFree(BLKSTART(parentBlk));
#elif CMK_CONVERSE_UGNI
    LrtsFree(BLKSTART(parentBlk));
#elif CONVERSE_POOL
    CmiPoolFree(BLKSTART(parentBlk));
#elif USE_MPI_CTRLMSG_SCHEME && CMK_CONVERSE_MPI
    MPI_Free_mem(parentBlk);
#elif CMK_SMP && CMK_BLUEGENEQ && SPECIFIC_PCQUEUE
    CmiFree_bgq(BLKSTART(parentBlk));
#elif CMK_SMP && CMK_PPC_ATOMIC_QUEUE
    CmiFree_ppcq(BLKSTART(parentBlk));
#else
    free_nomigrate(BLKSTART(parentBlk));
#endif
  }
}


/***************************************************************************
 *
 * Temporary-memory Allocation routines 
 *
 *  This buffer augments the storage available on the regular machine stack
 * for fairly large temporary buffers, which allows us to use smaller machine
 * stacks.
 *
 ***************************************************************************/

#define CMI_TMP_BUF_MAX 128*1024 /* Allow this much temporary storage. */

typedef struct {
  char *buf; /* Start of temporary buffer */
  int cur; /* First unused location in temporary buffer */
  int max; /* Length of temporary buffer */
} CmiTmpBuf_t;
CpvDeclare(CmiTmpBuf_t,CmiTmpBuf); /* One temporary buffer per PE */

static void CmiTmpSetup(CmiTmpBuf_t *b) {
  b->buf=malloc(CMI_TMP_BUF_MAX);
  b->cur=0;
  b->max=CMI_TMP_BUF_MAX;
}

void *CmiTmpAlloc(int size) {
  if (!CpvInitialized(CmiTmpBuf)) {
    return malloc(size);
  }
  else { /* regular case */
    CmiTmpBuf_t *b=&CpvAccess(CmiTmpBuf);
    void *t;
    if (b->cur+size>b->max) {
      if (b->max==0) /* We're just uninitialized */
        CmiTmpSetup(b);
      else /* We're really out of space! */
        CmiAbort("CmiTmpAlloc: asked for too much temporary buffer space");
    }
    t=b->buf+b->cur;
    b->cur+=size;
    return t;
  }
}
void CmiTmpFree(void *t) {
  if (!CpvInitialized(CmiTmpBuf)) {
    free(t);
  }
  else { /* regular case */
    CmiTmpBuf_t *b=&CpvAccess(CmiTmpBuf);
    /* t should point into our temporary buffer: figure out where */
    int cur=((const char *)t)-b->buf;
#if CMK_ERROR_CHECKING
    if (cur<0 || cur>b->max)
      CmiAbort("CmiTmpFree: called with an invalid pointer");
#endif
    b->cur=cur;
  }
}

void CmiTmpInit(char **argv) {
  CpvInitialize(CmiTmpBuf_t,CmiTmpBuf);
  /* Set up this processor's temporary buffer */
  CmiTmpSetup(&CpvAccess(CmiTmpBuf));
}

/******************************************************************************

  Cross-platform directory creation

  ****************************************************************************/
#ifdef _MSC_VER
/* Windows directory creation: */
#include <windows.h>

void CmiMkdir(const char *dirName) {
	CreateDirectory(dirName,NULL);
}

#else /* !_MSC_VER */
/* UNIX directory creation */
#include <unistd.h> 
#include <sys/stat.h> /* from "mkdir" man page */
#include <sys/types.h>

void CmiMkdir(const char *dirName) {
#ifndef __MINGW_H
	mkdir(dirName,0777);
#else
	mkdir(dirName);
#endif
}

#endif


/******************************************************************************

  Multiple Send function                               

  ****************************************************************************/





/****************************************************************************
* DESCRIPTION : This function call allows the user to send multiple messages
*               from one processor to another, all intended for differnet 
*	        handlers.
*
*	        Parameters :
*
*	        destPE, len, int sizes[0..len-1], char *messages[0..len-1]
*
****************************************************************************/
/* Round up message size to the message granularity. 
   Does this by adding, then truncating.
*/
static int roundUpSize(unsigned int s) {
  return (int)((s+sizeof(double)-1)&~(sizeof(double)-1));
}
/* Return the amount of message padding required for a message
   with this many user bytes. 
 */
static int paddingSize(unsigned int s) {
  return roundUpSize(s)-s;
}

/* Message header for a bundle of multiple-sent messages */
typedef struct {
  char convHeader[CmiMsgHeaderSizeBytes];
  int nMessages; /* Number of distinct messages bundled below. */
  double pad; /* To align the first message, which follows this header */
} CmiMultipleSendHeader;

#if CMK_USE_IBVERBS | CMK_USE_IBUD
/* given a pointer to a multisend message clean up the metadata */

void infi_freeMultipleSend(void *msgWhole)
{
  int len=((CmiMultipleSendHeader *)msgWhole)->nMessages;
  double pad=((CmiMultipleSendHeader *)msgWhole)->pad;
  int offset=sizeof(CmiMultipleSendHeader);
  int m;
  void *thisMsg=NULL;
  if (pad != 1234567.89) return;
  for(m=0;m<len;m++)
    {
      /*unreg meta, free meta, move the ptr */
      /* note these weird little things are not pooled */
      /* do NOT free the message here, we are only a part of this buffer*/
      infiCmiChunkHeader *ch=(infiCmiChunkHeader *)(msgWhole+offset);
      char *msg=(msgWhole+offset+sizeof(infiCmiChunkHeader));
      int msgSize=ch->chunkHeader.size; /* Size of user portion of message (plus padding at end) */
      infi_unregAndFreeMeta(ch->metaData);
      offset+= sizeof(infiCmiChunkHeader) + msgSize;
    }
}
#endif


static void _CmiMultipleSend(unsigned int destPE, int len, int sizes[], char *msgComps[], int immed)
{
  CmiMultipleSendHeader header;
  int m; /* Outgoing message */

#if CMK_USE_IBVERBS
  infiCmiChunkHeader *msgHdr;
#else
  CmiChunkHeader *msgHdr; /* Chunk headers for each message */
#endif
	
  double pad = 0; /* padding required */
  int vecLen; /* Number of pieces in outgoing message vector */
  int *vecSizes; /* Sizes of each piece we're sending out. */
  char **vecPtrs; /* Pointers to each piece we're sending out. */
  int vec; /* Entry we're currently filling out in above array */
	
#if CMK_USE_IBVERBS
  msgHdr = (infiCmiChunkHeader *)CmiTmpAlloc(len * sizeof(infiCmiChunkHeader));
#else
  msgHdr = (CmiChunkHeader *)CmiTmpAlloc(len * sizeof(CmiChunkHeader));
#endif
	
  /* Allocate memory for the outgoing vector*/
  vecLen=1+3*len; /* Header and 3 parts per message */
  vecSizes = (int *)CmiTmpAlloc(vecLen * sizeof(int));
  vecPtrs = (char **)CmiTmpAlloc(vecLen * sizeof(char *));
  vec=0;
  
  /* Build the header */
  header.nMessages=len;
  CmiSetHandler(&header, CpvAccess(CmiMainHandlerIDP));
  header.pad = 1234567.89;
#if CMK_IMMEDIATE_MSG
  if (immed) CmiBecomeImmediate(&header);
#endif
  vecSizes[vec]=sizeof(header); vecPtrs[vec]=(char *)&header;
  vec++;

  /* Build an entry for each message: 
         | CmiChunkHeader | Message data | Message padding | ...next message entry ...
  */
  for (m=0;m<len;m++) {
#if CMK_USE_IBVERBS
    msgHdr[m].chunkHeader.size=roundUpSize(sizes[m]); /* Size of message and padding */
    msgHdr[m].chunkHeader.ref=0; /* Reference count will be filled out on receive side */
    msgHdr[m].metaData=NULL;
#else
    msgHdr[m].size=roundUpSize(sizes[m]); /* Size of message and padding */
    msgHdr[m].ref=0; /* Reference count will be filled out on receive side */
#endif		
    
    /* First send the message's CmiChunkHeader (for use on receive side) */
#if CMK_USE_IBVERBS
    vecSizes[vec]=sizeof(infiCmiChunkHeader);
#else
    vecSizes[vec]=sizeof(CmiChunkHeader); 
#endif		
		vecPtrs[vec]=(char *)&msgHdr[m];
    vec++;
    
    /* Now send the actual message data */
    vecSizes[vec]=sizes[m]; vecPtrs[vec]=msgComps[m];
    vec++;
    
    /* Now send padding to align the next message on a double-boundary */
    vecSizes[vec]=paddingSize(sizes[m]); vecPtrs[vec]=(char *)&pad;
    vec++;
  }
  CmiAssert(vec==vecLen);
  
  CmiSyncVectorSend(destPE, vecLen, vecSizes, vecPtrs);
  
  CmiTmpFree(vecPtrs); /* CmiTmp: Be sure to throw away in opposite order of allocation */
  CmiTmpFree(vecSizes);
  CmiTmpFree(msgHdr);
}

void CmiMultipleSend(unsigned int destPE, int len, int sizes[], char *msgComps[])
{
  _CmiMultipleSend(destPE, len, sizes, msgComps, 0);
}

void CmiMultipleIsend(unsigned int destPE, int len, int sizes[], char *msgComps[])
{
  _CmiMultipleSend(destPE, len, sizes, msgComps, 1);
}

/****************************************************************************
* DESCRIPTION : This function initializes the main handler required for the
*               CmiMultipleSend() function to work. 
*	        
*               This function should be called once in any Converse program
*	        that uses CmiMultipleSend()
*
****************************************************************************/

static void CmiMultiMsgHandler(char *msgWhole);

void CmiInitMultipleSend(void)
{
  CpvInitialize(int,CmiMainHandlerIDP); 
  CpvAccess(CmiMainHandlerIDP) =
    CmiRegisterHandler((CmiHandler)CmiMultiMsgHandler);
}

/****************************************************************************
* DESCRIPTION : This function is the main handler that splits up the messages
*               CmiMultipleSend() pastes together. 
*
****************************************************************************/

static void CmiMultiMsgHandler(char *msgWhole)
{
  int len=((CmiMultipleSendHeader *)msgWhole)->nMessages;
  int offset=sizeof(CmiMultipleSendHeader);
  int m;
  for (m=0;m<len;m++) {
#if CMK_USE_IBVERBS
    infiCmiChunkHeader *ch=(infiCmiChunkHeader *)(msgWhole+offset);
    char *msg=(msgWhole+offset+sizeof(infiCmiChunkHeader));
    int msgSize=ch->chunkHeader.size; /* Size of user portion of message (plus padding at end) */
    ch->chunkHeader.ref=msgWhole-msg; 
    ch->metaData =  registerMultiSendMesg(msg,msgSize);
#else
    CmiChunkHeader *ch=(CmiChunkHeader *)(msgWhole+offset);
    char *msg=(msgWhole+offset+sizeof(CmiChunkHeader));
    int msgSize=ch->size; /* Size of user portion of message (plus padding at end) */
    ch->ref=msgWhole-msg; 
#endif		
    /* Link new message to owner via a negative ref pointer */
    CmiReference(msg); /* Follows link & increases reference count of *msgWhole* */
    CmiSyncSendAndFree(CmiMyPe(), msgSize, msg);
#if CMK_USE_IBVERBS
    offset+= sizeof(infiCmiChunkHeader) + msgSize;
#else
    offset+= sizeof(CmiChunkHeader) + msgSize;
#endif		
  }
  /* Release our reference to the whole message.  The message will
     only actually be deleted once all its sub-messages are free'd as well. */
  CmiFree(msgWhole);
}

/****************************************************************************
* Hypercube broadcast message passing.
****************************************************************************/

int HypercubeGetBcastDestinations(int mype, int total_pes, int k, int *dest_pes) {
  int num_pes = 0;
  for ( ; k>=0; --k) {
    /* add the processor destination at level k if it exist */
    dest_pes[num_pes] = mype ^ (1<<k);
    if (dest_pes[num_pes] >= total_pes) {
      /* find the first proc in the other part of the current dimention */
      dest_pes[num_pes] &= (~0u)<<k;
      /* if the first proc there is over CmiNumPes() then there is no other
      	 dimension, otherwise if it is valid compute my correspondent in such
      	 a way to minimize the load for every processor */
      if (total_pes>dest_pes[num_pes]) dest_pes[num_pes] += (mype - (mype & ((~0u)<<k))) % (total_pes - dest_pes[num_pes]);
      }
    if (dest_pes[num_pes] < total_pes) {
      /* if the destination is in the acceptable range increment num_pes */
      ++num_pes;
    }
  }
  return num_pes;
}


/****************************************************************************
* DESCRIPTION : This function initializes the main handler required for the
*               Immediate message
*	        
*               This function should be called once in any Converse program
*
****************************************************************************/

int _immediateLock = 0; /* if locked, all immediate message handling will be delayed. */
int _immediateFlag = 0; /* if set, there is delayed immediate message. */

CpvDeclare(int, CmiImmediateMsgHandlerIdx); /* Main handler that is run on every node */

/* xdl is the real handler */
static void CmiImmediateMsgHandler(char *msg)
{
  CmiSetHandler(msg, CmiGetXHandler(msg));
  CmiHandleMessage(msg);
}

void CmiInitImmediateMsg(void)
{
  CpvInitialize(int,CmiImmediateMsgHandlerIdx); 
  CpvAccess(CmiImmediateMsgHandlerIdx) =
    CmiRegisterHandler((CmiHandler)CmiImmediateMsgHandler);
}

/*#if !CMK_IMMEDIATE_MSG
#if !CMK_MACHINE_PROGRESS_DEFINED
void CmiProbeImmediateMsg()
{
}
#endif
#endif*/

/******** Idle timeout module (+idletimeout=30) *********/

typedef struct {
  int idle_timeout;/*Milliseconds to wait idle before aborting*/
  int is_idle;/*Boolean currently-idle flag*/
  int call_count;/*Number of timeout calls currently in flight*/
} cmi_cpu_idlerec;

static void on_timeout(cmi_cpu_idlerec *rec,double curWallTime)
{
  rec->call_count--;
  if(rec->call_count==0 && rec->is_idle==1) {
    CmiError("Idle time on PE %d exceeded specified timeout.\n", CmiMyPe());
    CmiAbort("Exiting.\n");
  }
}
static void on_idle(cmi_cpu_idlerec *rec,double curWallTime)
{
  CcdCallFnAfter((CcdVoidFn)on_timeout, rec, rec->idle_timeout);
  rec->call_count++; /*Keeps track of overlapping timeout calls.*/  
  rec->is_idle = 1;
}
static void on_busy(cmi_cpu_idlerec *rec,double curWallTime)
{
  rec->is_idle = 0;
}
static void CIdleTimeoutInit(char **argv)
{
  int idle_timeout=0; /*Seconds to wait*/
  CmiGetArgIntDesc(argv,"+idle-timeout",&idle_timeout,"Abort if idle for this many seconds");
  if(idle_timeout != 0) {
    cmi_cpu_idlerec *rec=(cmi_cpu_idlerec *)malloc(sizeof(cmi_cpu_idlerec));
    _MEMCHECK(rec);
    rec->idle_timeout=idle_timeout*1000;
    rec->is_idle=0;
    rec->call_count=0;
    CcdCallOnCondition(CcdPROCESSOR_BEGIN_IDLE, (CcdVoidFn)on_idle, rec);
    CcdCallOnCondition(CcdPROCESSOR_BEGIN_BUSY, (CcdVoidFn)on_busy, rec);
  }
}

/*****************************************************************************
 *
 * Converse Initialization
 *
 *****************************************************************************/

extern void CrnInit(void);
extern void CmiIsomallocInit(char **argv);
#if ! CMK_CMIPRINTF_IS_A_BUILTIN
void CmiIOInit(char **argv);
#endif

/* defined in cpuaffinity.c */
extern void CmiInitCPUAffinityUtil();

static void CmiProcessPriority(char **argv)
{
  int dummy, nicelevel=-100;      /* process priority */
  CmiGetArgIntDesc(argv,"+nice",&nicelevel,"Set the process priority level");
  /* ignore others */
  while (CmiGetArgIntDesc(argv,"+nice",&dummy,"Set the process priority level"));
  /* call setpriority once on each process to set process's priority */
  if (CmiMyRank() == 0 && nicelevel != -100)  {
#ifndef _WIN32
    if (0!=setpriority(PRIO_PROCESS, 0, nicelevel))  {
      CmiPrintf("[%d] setpriority failed with value %d. \n", CmiMyPe(), nicelevel);
      perror("setpriority");
      CmiAbort("setpriority failed.");
    }
    else
      CmiPrintf("[%d] Charm++: setpriority %d\n", CmiMyPe(), nicelevel);
#else
    HANDLE hProcess = GetCurrentProcess();
    DWORD dwPriorityClass = NORMAL_PRIORITY_CLASS;
    char *prio_str = "NORMAL_PRIORITY_CLASS";
    BOOL status;
    /*
       <-20:      real time
       -20--10:   high 
       -10-0:     above normal
       0:         normal
       0-10:      below normal
       10-:       idle
    */
    if (0) ;
#ifdef BELOW_NORMAL_PRIORITY_CLASS
    else if (nicelevel<10 && nicelevel>0) {
      dwPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
      prio_str = "BELOW_NORMAL_PRIORITY_CLASS";
    }
#endif
    else if (nicelevel>0) {
      dwPriorityClass = IDLE_PRIORITY_CLASS;
      prio_str = "IDLE_PRIORITY_CLASS";
    }
    else if (nicelevel<=-20) {
      dwPriorityClass = REALTIME_PRIORITY_CLASS;
      prio_str = "REALTIME_PRIORITY_CLASS";
    }
#ifdef ABOVE_NORMAL_PRIORITY_CLASS
    else if (nicelevel>-10 && nicelevel<0) {
      dwPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
      prio_str = "ABOVE_NORMAL_PRIORITY_CLASS";
    }
#endif
    else if (nicelevel<0) {
      dwPriorityClass = HIGH_PRIORITY_CLASS;
      prio_str = "HIGH_PRIORITY_CLASS";
    }
    status = SetPriorityClass(hProcess, dwPriorityClass);
    if (!status)  {
        int err=GetLastError();
        CmiPrintf("SetPriorityClass failed errno=%d, WSAerr=%d\n",errno, err);
        CmiAbort("SetPriorityClass failed.");
    }
    else
      CmiPrintf("[%d] Charm++: setpriority %s\n", CmiMyPe(), prio_str);
#endif
  }
}

void CommunicationServerInit()
{
#if CMK_IMMEDIATE_MSG
  CQdCpvInit();
  CpvInitialize(int,CmiImmediateMsgHandlerIdx); 
#endif
}


static int testEndian(void)
{
        int test=0x1c;
        unsigned char *c=(unsigned char *)&test;
        if (c[sizeof(int)-1]==0x1c)
                /* Macintosh and most workstations are big-endian */
                return 1;   /* Big-endian machine */
        if (c[0]==0x1c)
                /* Intel x86 PC's, and DEC VAX are little-endian */
                return 0;  /* Little-endian machine */
        return -2;  /*Unknown integer type */
}

int CmiEndianness()
{
  static int _cmi_endianness = -1;
  if (_cmi_endianness == -1) _cmi_endianness = testEndian();
  CmiAssert(_cmi_endianness != -2);
  return  _cmi_endianness;
}

#if CMK_USE_TSAN
/* This fixes bug #713, which is caused by tsan deadlocking inside
 * a 'write' syscall inside a mutex. */
static void checkTSanOptions()
{
  char *env = getenv("TSAN_OPTIONS");

  if (!env ||
      !strstr(env, "log_path=") ||
      strstr(env, "log_path=stdout") ||
      strstr(env, "log_path=stderr")) {
    CmiAbort("TSAN output must be redirected to disk.\n"
             "Run this program with TSAN_OPTIONS=\"log_path=filename\"");
  }
}
#endif

#if CMK_CCS_AVAILABLE
int ccsRunning;
#endif

int quietModeRequested;  // user has requested quiet mode
int quietMode; // quiet mode active (CmiPrintf's are disabled)

/**
  Main Converse initialization routine.  This routine is 
  called by the machine file (machine.c) to set up Converse.
  It's "Common" because it's shared by all the machine.c files. 
  
  The main task of this routine is to set up all the Cpv's
  (message queues, handler tables, etc.) used during main execution.
  
  On SMP versions, this initialization routine is called by 
  *all* processors of a node simultaniously.  It's *also* called
  by the communication thread, which is rather strange but needed
  for immediate messages.  Each call to this routine expects a 
  different copy of the argv arguments, so use CmiCopyArgs(argv).
  
  Requires:
    - A working network layer.
    - Working Cpv's and CmiNodeBarrier.
    - CthInit to already have been called.  CthInit is called
      from the machine layer directly, because some machine layers
      (like uth) use Converse threads internally.

  Initialization is somewhat subtle, in that various modules
  won't work properly until they're initialized.  For example,
  nobody can register handlers before calling CmiHandlerInit.
*/
void ConverseCommonInit(char **argv)
{
  CpvInitialize(int, _urgentSend);
  CpvAccess(_urgentSend) = 0;
  CpvInitialize(int,interopExitFlag);
  CpvAccess(interopExitFlag) = 0;

  CpvInitialize(int,_curRestartPhase);
  CpvAccess(_curRestartPhase)=1;
  CmiArgInit(argv);
  CmiMemoryInit(argv);
#if ! CMK_CMIPRINTF_IS_A_BUILTIN
  CmiIOInit(argv);
#endif
  if (CmiMyPe() == 0)
      CmiPrintf("Converse/Charm++ Commit ID: %s\n", CmiCommitID);

  CpvInitialize(int, cmiMyPeIdle);
  CpvAccess(cmiMyPeIdle) = 0;

/* #if CONVERSE_POOL */
  CmiPoolAllocInit(30);  
/* #endif */
  CmiTmpInit(argv);
  CmiTimerInit(argv);
  CstatsInit(argv);
  CmiInitCPUAffinityUtil();

  CcdModuleInit(argv);
  CmiHandlerInit();
  CmiReductionsInit();
  CIdleTimeoutInit(argv);
  
#if CMK_SHARED_VARS_POSIX_THREADS_SMP /*Used by the net-*-smp and multicore versions*/
  if(CmiGetArgFlagDesc(argv, "+CmiSpinOnIdle", "Force the runtime system to spin on message reception when idle, rather than sleeping")) {
    if(CmiMyRank() == 0) _Cmi_forceSpinOnIdle = 1;
  }
  if(CmiGetArgFlagDesc(argv, "+CmiSleepOnIdle", "Force the runtime system to sleep when idle, rather than spinning on message reception")) {
    if(CmiMyRank() == 0) _Cmi_sleepOnIdle = 1;
  }
  if(CmiGetArgFlagDesc(argv,"+CmiNoProcForComThread","Is there an extra processor for the communication thread on each node(only for net-smp-*) ?")){
    if (CmiMyPe() == 0) {
      CmiPrintf("Charm++> Note: The option +CmiNoProcForComThread has been superseded by +CmiSleepOnIdle\n");
    }
    if(CmiMyRank() == 0) _Cmi_sleepOnIdle=1;
  }
  if (_Cmi_sleepOnIdle && _Cmi_forceSpinOnIdle) {
    if(CmiMyRank() == 0) CmiAbort("The option +CmiSpinOnIdle is mutually exclusive with the options +CmiSleepOnIdle and +CmiNoProcForComThread");
  }
#endif
	
#if CMK_TRACE_ENABLED
  traceInit(argv);
/*initTraceCore(argv);*/ /* projector */
#endif
  CmiProcessPriority(argv);

#if CMK_USE_TSAN
  checkTSanOptions();
#endif

  CmiPersistentInit();
  CmiIsomallocInit(argv);
  CmiDeliversInit();
  CsdInit(argv);
#if CMK_CCS_AVAILABLE
  ccsRunning = 0;
  CcsInit(argv);
#endif
  CpdInit();
  CthSchedInit();
  CmiGroupInit();
  CmiMulticastInit();
  CmiInitMultipleSend();
  CQdInit();

  CrnInit();
  CmiInitImmediateMsg();
  CldModuleInit(argv);
  
#if CMK_CELL
  void CmiInitCell();
  CmiInitCell();
#endif

#ifdef CMK_CUDA
  initHybridAPI(CmiMyPe()); 
#endif

  /* main thread is suspendable */
/*
  CthSetSuspendable(CthSelf(), 0);
*/

#if CMK_BIGSIM_CHARM
   /* have to initialize QD here instead of _initCharm */
  initQd(argv);
#endif
}

void ConverseCommonExit(void)
{
  CcsImpl_kill();

#if CMK_TRACE_ENABLED
  traceClose();
/*closeTraceCore();*/ /* projector */
#endif

#if CMI_IO_BUFFER_EXPLICIT
  CmiFlush(stdout);  /* end of program, always flush */
#endif

#if CMK_CELL
  CloseOffloadAPI();
#endif

#if CMK_CUDA
  exitHybridAPI(); 
#endif
  seedBalancerExit();
  EmergencyExit();
}


#if CMK_CELL != 0

extern void register_accel_spe_funcs(void);

void CmiInitCell()
{
  // Create a unique string for each PPE to use for the timing
  //   data file's name
  char fileNameBuf[64];
  sprintf(fileNameBuf, "speTiming.%d", CmiMyPe());

  InitOffloadAPI(offloadCallback, NULL, NULL, fileNameBuf);
  //CcdCallOnConditionKeep(CcdPERIODIC, 
  //      (CcdVoidFn) OffloadAPIProgress, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,
      (CcdVoidFn) OffloadAPIProgress, NULL);

  // Register accelerated entry methods on the PPE
  register_accel_spe_funcs();
}

#include "cell-api.c"

#endif

/****
 * CW Lee - 9/14/2005
 * Added a mechanism to allow some control over machines with extremely
 * inefficient terminal IO mechanisms. Case in point: the XT3 has a
 * 20ms flush overhead along with about 25MB/s bandwidth for IO. This,
 * coupled with a default setup using unbuffered stdout introduced
 * severe overheads (and hence limiting scaling) for applications like 
 * NAMD.
 */
#if ! CMK_CMIPRINTF_IS_A_BUILTIN
void CmiIOInit(char **argv) {
  CpvInitialize(int, expIOFlushFlag);
#if CMI_IO_BUFFER_EXPLICIT
  /* 
     Support for an explicit buffer only makes sense if the machine
     layer does not wish to make its own implementation.

     Placing this after CmiMemoryInit() means that CmiMemoryInit()
     MUST NOT make use of stdout if an explicit buffer is requested.

     The setvbuf function may only be used after opening a stream and
     before any other operations have been performed on it
  */
  CpvInitialize(char*, explicitIOBuffer);
  CpvInitialize(int, expIOBufferSize);
  if (!CmiGetArgIntDesc(argv,"+io_buffer_size", &CpvAccess(expIOBufferSize),
			"Explicit IO Buffer Size")) {
    CpvAccess(expIOBufferSize) = DEFAULT_IO_BUFFER_SIZE;
  }
  if (CpvAccess(expIOBufferSize) <= 0) {
    CpvAccess(expIOBufferSize) = DEFAULT_IO_BUFFER_SIZE;
  }
  CpvAccess(explicitIOBuffer) = (char*)CmiAlloc(CpvAccess(expIOBufferSize)*
						sizeof(char));
  if (setvbuf(stdout, CpvAccess(explicitIOBuffer), _IOFBF, 
	      CpvAccess(expIOBufferSize))) {
    CmiAbort("Explicit IO Buffering failed\n");
  }
#endif
#if CMI_IO_FLUSH_USER
  /* system default to have user control flushing of IO */
  /* Now look for user override */
  CpvAccess(expIOFlushFlag) = !CmiGetArgFlagDesc(argv,"+io_flush_system",
						 "System Controls IO Flush");
#else
  /* system default to have system handle IO flushing */
  /* Now look for user override */
  CpvAccess(expIOFlushFlag) = CmiGetArgFlagDesc(argv,"+io_flush_user",
						"User Controls IO Flush");
#endif
}
#endif

#if ! CMK_CMIPRINTF_IS_A_BUILTIN

void CmiPrintf(const char *format, ...)
{
  if (quietMode) return;
  CpdSystemEnter();
  {
  va_list args;
  va_start(args,format);
  vfprintf(stdout,format, args);
  if (CpvInitialized(expIOFlushFlag) && !CpvAccess(expIOFlushFlag)) {
    CmiFlush(stdout);
  }
  va_end(args);
#if CMK_CCS_AVAILABLE && CMK_CMIPRINTF_IS_A_BUILTIN
  if (CpvAccess(cmiArgDebugFlag)) {
    va_start(args,format);
    print_node0(format, args);
    va_end(args);
  }
#endif
  }
  CpdSystemExit();
}

void CmiError(const char *format, ...)
{
  CpdSystemEnter();
  {
  va_list args;
  va_start(args,format);
  vfprintf(stderr,format, args);
  CmiFlush(stderr);  /* stderr is always flushed */
  va_end(args);
#if CMK_CCS_AVAILABLE && CMK_CMIPRINTF_IS_A_BUILTIN
  if (CpvAccess(cmiArgDebugFlag)) {
    va_start(args,format);
    print_node0(format, args);
    va_end(args);
  }
#endif
  }
  CpdSystemExit();
}

#endif

void __cmi_assert(const char *errmsg)
{
  CmiError("[%d] %s\n", CmiMyPe(), errmsg);
  CmiAbort(errmsg);
}

char *CmiCopyMsg(char *msg, int len)
{
  char *copy = (char *)CmiAlloc(len);
  _MEMCHECK(copy);
  memcpy(copy, msg, len);
  return copy;
}

unsigned char computeCheckSum(unsigned char *data, int len)
{
  int i;
  unsigned char ret = 0;
  for (i=0; i<len; i++) ret ^= (unsigned char)data[i];
  return ret;
}

/* Flag for bigsim's out-of-core emulation */
int _BgOutOfCoreFlag=0; /*indicate the type of memory operation (in or out) */
int _BgInOutOfCoreMode=0; /*indicate whether the emulation is in the out-of-core emulation mode */

#if !CMK_HAS_LOG2
unsigned int CmiILog2(unsigned int val) {
  unsigned int log = 0u;
  if ( val != 0u ) {
      while ( val > (1u<<log) ) { log++; }
  }
  return log;
}
double CmiLog2(double x) {
  return log(x)/log(2);
}
#endif

/* for bigsim */
int CmiMyRank_()
{
  return CmiMyRank();
}

double CmiReadSize(const char *str)
{
    double val;
    if (strpbrk(str,"Gg")) {
        //sscanf(str, "%llf", &val);
        //val = strtod(str, &p);
        val = atof(str);
        val *= 1024ll*1024*1024;
    }
    else if (strpbrk(str,"Mm")) {
        val = atof(str);
        val *= 1024*1024;
    }
    else if (strpbrk(str,"Kk")) {
        val = atof(str);
        val *= 1024;
    }
    else {
        val = atof(str);
    }
    return val;
}

int CmiIsMyNodeIdle(){
    int i;
    for(i=0; i<CmiMyNodeSize(); i++){
        if(CpvAccessOther(cmiMyPeIdle, i)) return 1;
    }
    return 0;
}

/*@}*/
