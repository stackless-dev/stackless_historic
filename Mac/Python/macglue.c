/***********************************************************
Copyright 1991-1997 by Stichting Mathematisch Centrum, Amsterdam,
The Netherlands.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its 
documentation for any purpose and without fee is hereby granted, 
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in 
supporting documentation, and that the names of Stichting Mathematisch
Centrum or CWI not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior permission.

STICHTING MATHEMATISCH CENTRUM DISCLAIMS ALL WARRANTIES WITH REGARD TO
THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS, IN NO EVENT SHALL STICHTING MATHEMATISCH CENTRUM BE LIABLE
FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

******************************************************************/

#ifdef __CFM68K__
/* cfm68k InterfaceLib exports GetEventQueue, but Events.h doesn't know this
** and defines it as GetEvQHdr (which is correct for PPC). This fix is for
** CW9, check that the workaround is still needed for the next release.
*/
#define GetEvQHdr GetEventQueue
#endif /* __CFM68K__ */

#include <Events.h>

#if TARGET_API_MAC_CARBON
/* Unfortunately this call is probably slower... */
#define LMGetTicks() TickCount()
#endif

#ifdef __CFM68K__
#undef GetEventQueue
#endif /* __CFM68K__ */

#include "Python.h"

#include "macglue.h"
#include "marshal.h"
#include "import.h"
#include "importdl.h"

#include "pythonresources.h"

#include <OSUtils.h> /* for Set(Current)A5 */
#include <Files.h>
#include <StandardFile.h>
#include <Resources.h>
#include <Memory.h>
#include <Windows.h>
#include <Traps.h>
#include <Processes.h>
#include <Fonts.h>
#include <Menus.h>
#include <TextUtils.h>
#ifdef __MWERKS__
#include <SIOUX.h>
extern void SIOUXSetupMenus(void);
extern void SIOUXDoAboutBox(void);
#endif
#ifdef USE_GUSI
/* Functions we redefine because they're in obscure libraries */
extern void SpinCursor(short x);
extern void RotateCursor(short x);
extern pascal void PLstrcpy(unsigned char *, unsigned char *);
extern pascal int PLstrcmp(unsigned char *, unsigned char *);
extern pascal unsigned char *PLstrrchr(unsigned char *, unsigned char);

#endif

#ifdef USE_GUSI1
#include <TFileSpec.h> /* For Path2FSSpec */
#include <GUSI.h>
#endif
#include <LowMem.h>

/* The ID of the Sioux apple menu */
#define SIOUX_APPLEID	32000

#include <signal.h>
#include <stdio.h>

/*
** When less than this amount of stackspace is left we
** raise a MemoryError.
*/
#ifndef MINIMUM_STACK_SIZE
#ifdef __powerc
#define MINIMUM_STACK_SIZE 8192
#else
#define MINIMUM_STACK_SIZE 4096
#endif
#endif

/*
** We have to be careful, since we can't handle
** things like updates (and they'll keep coming back if we don't
** handle them). Note that we don't know who has windows open, so
** even handing updates off to SIOUX under MW isn't going to work.
*/
#define MAINLOOP_EVENTMASK (mDownMask|keyDownMask|osMask|activMask)

#include <signal.h>

/* XXX We should include Errors.h here, but it has a name conflict
** with the python errors.h. */
#define fnfErr -43

/* Declared in macfsmodule.c: */
extern FSSpec *mfs_GetFSSpecFSSpec(PyObject *);
extern PyObject *newmfssobject(FSSpec *);

/* Interrupt code variables: */
static int interrupted;			/* Set to true when cmd-. seen */
static RETSIGTYPE intcatcher(int);

static int PyMac_Yield(void);

/*
** These are the real scheduling parameters that control what we check
** in the event loop, and how often we check. The values are initialized
** from pyMac_SchedParamStruct.
*/

struct real_sched_param_struct {
	int		check_interrupt;	/* if true check for command-dot */
	int		process_events;		/* if nonzero enable evt processing, this mask */
	int		besocial;		/* if nonzero be a little social with CPU */
	unsigned long	check_interval;		/* how often to check, in ticks */
	unsigned long	bg_yield;		/* yield so long when in background */
	/* these are computed from previous and clock and such */
	int		enabled;		/* check_interrupt OR process_event OR yield */
	unsigned long	next_check;		/* when to check/yield next, in ticks */
};

static struct real_sched_param_struct schedparams =
	{ 1, MAINLOOP_EVENTMASK, 1, 15, 15, 1, 0};

/*
** Workaround for sioux/gusi combo: set when we are exiting
*/
int PyMac_ConsoleIsDead;

/*
** Sioux menu bar, saved early so we can restore it
*/
static Handle sioux_mbar;

/*
** Some stuff for our GetDirectory and PromptGetFile routines
*/
struct hook_args {
	int selectcur_hit;		/* Set to true when "select current" selected */
	char *prompt;			/* The prompt */
};
#if TARGET_API_MAC_CARBON
/* The StandardFile hooks don't exist in Carbon. This breaks GetDirectory,
** but the macfsn code will replace it by a NavServices version anyway.
*/
#define myhook_upp NULL
#else
static DlgHookYDUPP myhook_upp;
static int upp_inited = 0;
#endif

/*
** The python-code event handler
*/
static PyObject *python_event_handler;

/*
** Set to true if we're appearance-compliant
*/
int PyMac_AppearanceCompliant;

/*
** Find out what the current script is.
** Donated by Fredrik Lund.
*/
char *PyMac_getscript()
{
   int font, script, lang;
    font = 0;
    font = GetSysFont();
    script = FontToScript(font);
    switch (script) {
    case smRoman:
        lang = GetScriptVariable(script, smScriptLang);
        if (lang == langIcelandic)
            return "mac-iceland";
        else if (lang == langTurkish)
            return "mac-turkish";
        else if (lang == langGreek)
            return "mac-greek";
        else
            return "mac-roman";
        break;
    case smJapanese:
        return "mac-japan";
    case smGreek:
        return "mac-greek";
    case smCyrillic:
        return "mac-cyrillic";
    default:
        return "mac-roman"; /* better than nothing */
    }
}


#ifdef USE_GUSI1
/*
** GUSI (1.6.0 and earlier, at the least) do not set the MacOS idea of
** the working directory. Hence, we call this routine after each call
** to chdir() to rectify things.
*/
void
PyMac_FixGUSIcd()
{
	WDPBRec pb;
	FSSpec curdirfss;
	
	if ( Path2FSSpec(":x", &curdirfss) != noErr ) 
		return;
	
	/* Set MacOS "working directory" */
	pb.ioNamePtr= "\p";
	pb.ioVRefNum= curdirfss.vRefNum;
	pb.ioWDDirID= curdirfss.parID;
	if (PBHSetVolSync(&pb) != noErr)
		return;
}
#endif

#ifdef USE_GUSI
/*
** SpinCursor (needed by GUSI) drags in heaps of stuff, so we
** provide a dummy here.
*/
void SpinCursor(short x) { /* Dummy */ }
void RotateCursor(short x) { /* Dummy */ }

/*
** Replacement GUSI Spin function
*/
#ifdef USE_GUSI1
static int
PyMac_GUSISpin(spin_msg msg, long arg)
{
	static Boolean	inForeground = true;
	int		maxsleep = 6;	/* 6 ticks is "normal" sleeptime */

	if (PyMac_ConsoleIsDead) return 0;
#if 0
	if (inForeground)
		SpinCursor(msg == SP_AUTO_SPIN ? short(arg) : 1);
#endif

	if (interrupted) return -1;

	if ( msg == SP_AUTO_SPIN )
		maxsleep = 0;
	if ( msg==SP_SLEEP||msg==SP_SELECT )
		maxsleep = arg;

	PyMac_DoYield(maxsleep, 0); /* XXXX or is it safe to call python here? */

	return 0;
}

void
PyMac_SetGUSISpin() {
	GUSISetHook(GUSI_SpinHook, (GUSIHook)PyMac_GUSISpin);
}
#endif

/* Called at exit() time thru atexit(), to stop event processing */
void
PyMac_StopGUSISpin() {
	PyMac_ConsoleIsDead = 1;
}

/*
** Replacement routines for the PLstr... functions so we don't need
** StdCLib. Moreover, that implementation is broken under cfm68k...
*/
pascal void
PLstrcpy(unsigned char *to, unsigned char *fr)
{
	memcpy(to, fr, fr[0]+1);
}

pascal int
PLstrcmp(unsigned char *s1, unsigned char *s2)
{
	int res;
	int l = s1[0] < s2[0] ? s1[0] : s2[0];
	
	res = memcmp(s1+1, s2+1, l);
	if ( res != 0 )
		return res;
	
	if ( s1[0] < s2[0] )
		return -1;
	else if ( s1[0] > s2[0] )
		return 1;
	else
		return 0;
}

pascal unsigned char *
PLstrrchr(unsigned char *str, unsigned char chr)
{
	unsigned char *ptr = 0;
	unsigned char *p;
	
	for(p=str+1; p<str+str[0]; p++)
		if ( *p == chr )
			ptr = p;
	return ptr;
}
	
#endif /* USE_GUSI */


/* Convert C to Pascal string. Returns pointer to static buffer. */
unsigned char *
Pstring(char *str)
{
	static Str255 buf;
	int len;

	len = strlen(str);
	if (len > 255)
		len = 255;
	buf[0] = (unsigned char)len;
	strncpy((char *)buf+1, str, len);
	return buf;
}

void
c2pstrcpy(unsigned char *dst, const char *src)
{
	int len;
	
	len = strlen(src);
	if ( len > 255 ) len = 255;
	strncpy((char *)dst+1, src, len);
	dst[0] = len;
}

/* Like strerror() but for Mac OS error numbers */
char *PyMac_StrError(int err)
{
	static char buf[256];
	Handle h;
	char *str;
	
	h = GetResource('Estr', err);
	if ( h ) {
		HLock(h);
		str = (char *)*h;
		memcpy(buf, str+1, (unsigned char)str[0]);
		buf[(unsigned char)str[0]] = '\0';
		HUnlock(h);
		ReleaseResource(h);
	} else {
		sprintf(buf, "Mac OS error code %d", err);
	}
	return buf;
}

/* Exception object shared by all Mac specific modules for Mac OS errors */
PyObject *PyMac_OSErrException;

/* Initialize and return PyMac_OSErrException */
PyObject *
PyMac_GetOSErrException()
{
	if (PyMac_OSErrException == NULL)
		PyMac_OSErrException = PyString_FromString("Mac OS Error");
	return PyMac_OSErrException;
}

/* Set a MAC-specific error from errno, and return NULL; return None if no error */
PyObject * 
PyErr_Mac(PyObject *eobj, int err)
{
	char *msg;
	PyObject *v;
	
	if (err == 0 && !PyErr_Occurred()) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (err == -1 && PyErr_Occurred())
		return NULL;
	msg = PyMac_StrError(err);
	v = Py_BuildValue("(is)", err, msg);
	PyErr_SetObject(eobj, v);
	Py_DECREF(v);
	return NULL;
}

/* Call PyErr_Mac with PyMac_OSErrException */
PyObject *
PyMac_Error(OSErr err)
{
	return PyErr_Mac(PyMac_GetOSErrException(), err);
}

#ifdef USE_STACKCHECK
/* Check for stack overflow */
int
PyOS_CheckStack()
{
	char here;
	static char *sentinel = 0;
	static PyThreadState *thread_for_sentinel = 0;
	
	if ( sentinel == 0 ) {		
		sentinel = &here - StackSpace() + MINIMUM_STACK_SIZE;
	}
	if ( thread_for_sentinel == 0 ) {
		thread_for_sentinel = PyThreadState_Get();
	}
	if ( &here < sentinel ) {
		if (thread_for_sentinel == PyThreadState_Get()) {
			return -1;
#if 0
		} else {
			/* Else we are unsure... */
			fprintf(stderr, "Stackcheck in other thread (was %x now %x)\n", thread_for_sentinel,PyThreadState_Get()); 
#endif
		}
	}
	return 0;
}
#endif /* USE_STACKCHECK */

/* The catcher routine (which may not be used for all compilers) */
static RETSIGTYPE
intcatcher(sig)
	int sig;
{
	interrupted = 1;
	signal(SIGINT, intcatcher);
}

void
PyOS_InitInterrupts()
{
	if (signal(SIGINT, SIG_IGN) != SIG_IGN)
		signal(SIGINT, intcatcher);
}

void
PyOS_FiniInterrupts()
{
}

/*
** This routine scans the event queue looking for cmd-.
** This is the only way to get an interrupt under THINK (since it
** doesn't do SIGINT handling), but is also used under MW, when
** the full-fledged event loop is disabled. This way, we can at least
** interrupt a runaway python program.
*/
static void
scan_event_queue(flush)
	int flush;
{
#if TARGET_API_MAC_CARBON
	/* CARBONXXXX To be implemented */
	return;
#else
	register EvQElPtr q;
	
	q = (EvQElPtr) LMGetEventQueue()->qHead;
	
	for (; q; q = (EvQElPtr)q->qLink) {
		if (q->evtQWhat == keyDown &&
				(char)q->evtQMessage == '.' &&
				(q->evtQModifiers & cmdKey) != 0) {
			if ( flush )
				FlushEvents(keyDownMask, 0);
			interrupted = 1;
			break;
		}
	}
#endif
}

int
PyErr_CheckSignals()
{
	if (schedparams.enabled) {
		if ( (unsigned long)LMGetTicks() > schedparams.next_check ) {
			if ( PyMac_Yield() < 0)
				return -1;
			schedparams.next_check = (unsigned long)LMGetTicks()
					 + schedparams.check_interval;
			if (interrupted) {
				scan_event_queue(1);	/* Eat events up to cmd-. */
				interrupted = 0;
				PyErr_SetNone(PyExc_KeyboardInterrupt);
				return -1;
			}
		}
	}
	return 0;
}

int
PyOS_InterruptOccurred()
{
	scan_event_queue(1);
	return interrupted;
}
/* Check whether we are in the foreground */
static int
PyMac_InForeground(void)
{
	static ProcessSerialNumber ours;
	static inited;
	ProcessSerialNumber curfg;
	Boolean eq;
	
	if ( inited == 0 ) {
		(void)GetCurrentProcess(&ours);
		inited = 1;
	}
	if ( GetFrontProcess(&curfg) < 0 )
		eq = 1;
	else if ( SameProcess(&ours, &curfg, &eq) < 0 )
		eq = 1;
	return (int)eq;

}

int
PyMac_SetEventHandler(PyObject *evh)
{
	if ( evh && python_event_handler ) {
		PyErr_SetString(PyExc_RuntimeError, "Python event handler already set");
		return 0;
	}
	if ( python_event_handler )
		Py_DECREF(python_event_handler);
	if ( evh )
		Py_INCREF(evh);
	python_event_handler = evh;
	return 1;
}

/*
** Handle an event, either one found in the mainloop eventhandler or
** one passed back from the python program.
*/
void
PyMac_HandleEventIntern(evp)
	EventRecord *evp;
{
#if !TARGET_API_MAC_CARBON
	if ( evp->what == mouseDown ) {
		WindowPtr wp;
		
		if ( FindWindow(evp->where, &wp) == inSysWindow ) {
			SystemClick(evp, wp);
			return;
		}
	}
#endif
#ifdef __MWERKS__
	{
		int siouxdidit;

		/* If SIOUX wants it we're done */
		siouxdidit = SIOUXHandleOneEvent(evp);
		if ( siouxdidit )
			return;
	}
#else
	/* Other compilers are just unlucky... */
#endif /* !__MWERKS__ */
}

/*
** Handle an event, either through HandleEvent or by passing it to the Python
** event handler.
*/
int
PyMac_HandleEvent(evp)
	EventRecord *evp;
{
	PyObject *rv;
	
	if ( python_event_handler ) {
		rv = PyObject_CallFunction(python_event_handler, "(O&)", 
			PyMac_BuildEventRecord, evp);
		if ( rv )
			Py_DECREF(rv);
		else
			return -1;	/* Propagate exception */
	} else {
		PyMac_HandleEventIntern(evp);
	}
	return 0;
}

/*
** Yield the CPU to other tasks without processing events.
*/
int
PyMac_DoYield(int maxsleep, int maycallpython)
{
	EventRecord ev;
	int gotone;
	long latest_time_ready;
	static int in_here = 0;
	
	in_here++;
	/*
	** First check for interrupts, if wanted.
	** This sets a flag that will be picked up at an appropriate
	** moment in the mainloop.
	*/
	if (schedparams.check_interrupt)
		scan_event_queue(0);
	
	/* XXXX Implementing an idle routine goes here */
		
	/*
	** Check which of the eventloop cases we have:
	** - process events
	** - don't process events but do yield
	** - do neither
	*/
	if( in_here > 1 || !schedparams.process_events || 
	    (python_event_handler && !maycallpython) ) {
#if !TARGET_API_MAC_CARBON
		if ( maxsleep >= 0 ) {
			SystemTask();
		}
#endif
	} else {
		latest_time_ready = LMGetTicks() + maxsleep;
		do {
			/* XXXX Hack by Jack.
			** In time.sleep() you can click to another application
			** once only. If you come back to Python you cannot get away
			** again.
			**/
			gotone = WaitNextEvent(schedparams.process_events, &ev, maxsleep, NULL);	
			/* Get out quickly if nothing interesting is happening */
			if ( !gotone || ev.what == nullEvent )
				break;
			if ( PyMac_HandleEvent(&ev) < 0 ) {
				in_here--;
				return -1;
			}
			maxsleep = latest_time_ready - LMGetTicks();
		} while ( maxsleep > 0 );
	}
	in_here--;
	return 0;
}

/*
** Process events and/or yield the CPU to other tasks if opportune
*/
int
PyMac_Yield() {
	unsigned long maxsleep;
	
	if( PyMac_InForeground() )
		maxsleep = 0;
	else
		maxsleep = schedparams.bg_yield;

	return PyMac_DoYield(maxsleep, 1);
}

/*
** Return current scheduler parameters
*/
void
PyMac_GetSchedParams(PyMacSchedParams *sp)
{
	sp->check_interrupt = schedparams.check_interrupt;
	sp->process_events = schedparams.process_events;
	sp->besocial = schedparams.besocial;
	sp->check_interval = schedparams.check_interval / 60.0;
	sp->bg_yield = schedparams.bg_yield / 60.0;
}

/*
** Set current scheduler parameters
*/
void
PyMac_SetSchedParams(PyMacSchedParams *sp)
{
	schedparams.check_interrupt = sp->check_interrupt;
	schedparams.process_events = sp->process_events;
	schedparams.besocial = sp->besocial;
	schedparams.check_interval = (unsigned long)(sp->check_interval*60);
	schedparams.bg_yield = (unsigned long)(sp->bg_yield*60);
	if ( schedparams.check_interrupt || schedparams.process_events ||
	     schedparams.besocial )
	     	schedparams.enabled = 1;
	else
		schedparams.enabled = 0;
	schedparams.next_check = 0;	/* Check immedeately */
}

/*
** Install our menu bar.
*/
void
PyMac_InitMenuBar()
{
	MenuHandle applemenu;
	
	if ( (sioux_mbar=GetMenuBar()) == NULL )  {
		/* Sioux menu not installed yet. Do so */
		SIOUXSetupMenus();
		if ( (sioux_mbar=GetMenuBar()) == NULL )
			return;
	}
	if ( (applemenu=GetMenuHandle(SIOUX_APPLEID)) == NULL ) return;
	SetMenuItemText(applemenu, 1, "\pAbout Python...");
}

/*
** Restore sioux menu bar
*/
void
PyMac_RestoreMenuBar()
{
	if ( sioux_mbar ) {
		SetMenuBar(sioux_mbar);
		DrawMenuBar();
	} else
		PyMac_InitMenuBar();
}


/*
** Our replacement about box
*/

#include "patchlevel.h"

void
SIOUXDoAboutBox(void)
{
	DialogPtr theDialog;
	WindowPtr theWindow;
	short item;
	short fontID;
	
	if( (theDialog = GetNewDialog(ABOUT_ID, NULL, (WindowPtr)-1)) == NULL )
		return;
	theWindow = GetDialogWindow(theDialog);
	SetPortWindowPort(theWindow);
	GetFNum("\pPython-Sans", &fontID);
	if (fontID == 0)
		fontID = kFontIDGeneva;
	TextFont(fontID);
	TextSize(9);
	ParamText(Pstring(PATCHLEVEL), "\p", "\p", "\p");
	ShowWindow(theWindow);
	ModalDialog(NULL, &item);
	DisposeDialog(theDialog);
}

#if 0
int
PyMac_FileExists(char *name)
{
	FSSpec fss;
	
	if ( FSMakeFSSpec(0, 0, Pstring(name), &fss) == noErr )
		return 1;
	return 0;
}
#endif

/*
** Helper routine for GetDirectory
*/
static pascal short
myhook_proc(short item, DialogPtr theDialog, struct hook_args *dataptr)
{
	if ( item == sfHookFirstCall && dataptr->prompt) {
		Handle prompth;
		short type;
		Rect rect;
		
		GetDialogItem(theDialog, PROMPT_ITEM, &type, &prompth, &rect);
		if ( prompth )
			SetDialogItemText(prompth, (unsigned char *)dataptr->prompt);
	} else
	if ( item == SELECTCUR_ITEM ) {
		item = sfItemCancelButton;
		dataptr->selectcur_hit = 1;
	}
	return item;
}	
#if !TARGET_API_MAC_CARBON
/*
** Ask the user for a directory. I still can't understand
** why Apple doesn't provide a standard solution for this...
*/
int
PyMac_GetDirectory(dirfss, prompt)
	FSSpec *dirfss;
	char *prompt;
{
	static SFTypeList list = {'fldr', 0, 0, 0};
	static Point where = {-1, -1};
	StandardFileReply reply;
	struct hook_args hook_args;
	
	if ( !upp_inited ) {
		myhook_upp = NewDlgHookYDProc(myhook_proc);
		upp_inited = 1;
	}
	if ( prompt && *prompt )
		hook_args.prompt = (char *)Pstring(prompt);
	else
		hook_args.prompt = NULL;
	hook_args.selectcur_hit = 0;
	CustomGetFile((FileFilterYDUPP)0, 1, list, &reply, GETDIR_ID, where, myhook_upp,
				NULL, NULL, NULL, (void *)&hook_args);
				
	reply.sfFile.name[0] = 0;
	if( FSMakeFSSpec(reply.sfFile.vRefNum, reply.sfFile.parID, reply.sfFile.name, dirfss) )
		return 0;
	return hook_args.selectcur_hit;
}

/*
** Slightly extended StandardGetFile: accepts a prompt */
void PyMac_PromptGetFile(short numTypes, ConstSFTypeListPtr typeList, 
		StandardFileReply *reply, char *prompt)
{
	static Point where = {-1, -1};
	struct hook_args hook_args;
	
	if ( !upp_inited ) {
		myhook_upp = NewDlgHookYDProc(myhook_proc);
		upp_inited = 1;
	}
	if ( prompt && *prompt )
		hook_args.prompt = (char *)Pstring(prompt);
	else
		hook_args.prompt = NULL;
	hook_args.selectcur_hit = 0;
	CustomGetFile((FileFilterYDUPP)0, numTypes, typeList, reply, GETFILEPROMPT_ID, where,
				myhook_upp, NULL, NULL, NULL, (void *)&hook_args);
}
#endif /* TARGET_API_MAC_CARBON */

/* Convert a 4-char string object argument to an OSType value */
int
PyMac_GetOSType(PyObject *v, OSType *pr)
{
	if (!PyString_Check(v) || PyString_Size(v) != 4) {
		PyErr_SetString(PyExc_TypeError,
			"OSType arg must be string of 4 chars");
		return 0;
	}
	memcpy((char *)pr, PyString_AsString(v), 4);
	return 1;
}

/* Convert an OSType value to a 4-char string object */
PyObject *
PyMac_BuildOSType(OSType t)
{
	return PyString_FromStringAndSize((char *)&t, 4);
}

/* Convert an NumVersion value to a 4-element tuple */
PyObject *
PyMac_BuildNumVersion(NumVersion t)
{
	return Py_BuildValue("(hhhh)", t.majorRev, t.minorAndBugRev, t.stage, t.nonRelRev);
}


/* Convert a Python string object to a Str255 */
int
PyMac_GetStr255(PyObject *v, Str255 pbuf)
{
	int len;
	if (!PyString_Check(v) || (len = PyString_Size(v)) > 255) {
		PyErr_SetString(PyExc_TypeError,
			"Str255 arg must be string of at most 255 chars");
		return 0;
	}
	pbuf[0] = len;
	memcpy((char *)(pbuf+1), PyString_AsString(v), len);
	return 1;
}

/* Convert a Str255 to a Python string object */
PyObject *
PyMac_BuildStr255(Str255 s)
{
	if ( s == NULL ) {
		PyErr_SetString(PyExc_SystemError, "Str255 pointer is NULL");
		return NULL;
	}
	return PyString_FromStringAndSize((char *)&s[1], (int)s[0]);
}

PyObject *
PyMac_BuildOptStr255(Str255 s)
{
	if ( s == NULL ) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	return PyString_FromStringAndSize((char *)&s[1], (int)s[0]);
}


/*
** Convert a Python object to an FSSpec.
** The object may either be a full pathname or a triple
** (vrefnum, dirid, path).
** NOTE: This routine will fail on pre-sys7 machines. 
** The caller is responsible for not calling this routine
** in those cases (which is fine, since everyone calling
** this is probably sys7 dependent anyway).
*/
int
PyMac_GetFSSpec(PyObject *v, FSSpec *fs)
{
	Str255 path;
	short refnum;
	long parid;
	OSErr err;
	FSSpec *fs2;

	/* first check whether it already is an FSSpec */
	fs2 = mfs_GetFSSpecFSSpec(v);
	if ( fs2 ) {
		(void)FSMakeFSSpec(fs2->vRefNum, fs2->parID, fs2->name, fs);
		return 1;
	}
	if ( PyString_Check(v) ) {
		/* It's a pathname */
		if( !PyArg_Parse(v, "O&", PyMac_GetStr255, &path) )
			return 0;
		refnum = 0; /* XXXX Should get CurWD here?? */
		parid = 0;
	} else {
		if( !PyArg_Parse(v, "(hlO&); FSSpec should be fullpath or (vrefnum,dirid,path)",
							&refnum, &parid, PyMac_GetStr255, &path)) {
			return 0;
		}
	}
	err = FSMakeFSSpec(refnum, parid, path, fs);
	if ( err && err != fnfErr ) {
		PyMac_Error(err);
		return 0;
	}
	return 1;
}

/* Convert FSSpec to PyObject */
PyObject *PyMac_BuildFSSpec(FSSpec *v)
{
	return newmfssobject(v);
}

/* Convert a Python object to a Rect.
   The object must be a (left, top, right, bottom) tuple.
   (This differs from the order in the struct but is consistent with
   the arguments to SetRect(), and also with STDWIN). */
int
PyMac_GetRect(PyObject *v, Rect *r)
{
	return PyArg_Parse(v, "(hhhh)", &r->left, &r->top, &r->right, &r->bottom);
}

/* Convert a Rect to a Python object */
PyObject *
PyMac_BuildRect(Rect *r)
{
	return Py_BuildValue("(hhhh)", r->left, r->top, r->right, r->bottom);
}


/* Convert a Python object to a Point.
   The object must be a (h, v) tuple.
   (This differs from the order in the struct but is consistent with
   the arguments to SetPoint(), and also with STDWIN). */
int
PyMac_GetPoint(PyObject *v, Point *p)
{
	return PyArg_Parse(v, "(hh)", &p->h, &p->v);
}

/* Convert a Point to a Python object */
PyObject *
PyMac_BuildPoint(Point p)
{
	return Py_BuildValue("(hh)", p.h, p.v);
}


/* Convert a Python object to an EventRecord.
   The object must be a (what, message, when, (v, h), modifiers) tuple. */
int
PyMac_GetEventRecord(PyObject *v, EventRecord *e)
{
	return PyArg_Parse(v, "(hll(hh)h)",
	                   &e->what,
	                   &e->message,
	                   &e->when,
	                   &e->where.h,
	                   &e->where.v,                   
	                   &e->modifiers);
}

/* Convert a Rect to an EventRecord object */
PyObject *
PyMac_BuildEventRecord(EventRecord *e)
{
	return Py_BuildValue("(hll(hh)h)",
	                     e->what,
	                     e->message,
	                     e->when,
	                     e->where.h,
	                     e->where.v,
	                     e->modifiers);
}

/* Convert Python object to Fixed */
int
PyMac_GetFixed(PyObject *v, Fixed *f)
{
	double d;
	
	if( !PyArg_Parse(v, "d", &d))
		return 0;
	*f = (Fixed)(d * 0x10000);
	return 1;
}

/* Convert a Point to a Python object */
PyObject *
PyMac_BuildFixed(Fixed f)
{
	double d;
	
	d = f;
	d = d / 0x10000;
	return Py_BuildValue("d", d);
}

/* Convert wide to/from Python int or (hi, lo) tuple. XXXX Should use Python longs */
int
PyMac_Getwide(PyObject *v, wide *rv)
{
	if (PyInt_Check(v)) {
		rv->hi = 0;
		rv->lo = PyInt_AsLong(v);
		if( rv->lo & 0x80000000 )
			rv->hi = -1;
		return 1;
	}
	return PyArg_Parse(v, "(ll)", &rv->hi, &rv->lo);
}


PyObject *
PyMac_Buildwide(wide *w)
{
	if ( (w->hi == 0 && (w->lo & 0x80000000) == 0) ||
	     (w->hi == -1 && (w->lo & 0x80000000) ) )
		return PyInt_FromLong(w->lo);
	return Py_BuildValue("(ll)", w->hi, w->lo);
}
