////////////////////////////////////////////////////////////////////////////////
//  $Id: vld.cpp,v 1.2 2005/03/29 14:18:03 db Exp $
//
//  Visual Leak Detector (Version 0.9d)
//  Copyright (c) 2005 Dan Moulding
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU Lesser Public License as published by
//  the Free Software Foundation; either version 2.1 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser Public License for more details.
//
//  You should have received a copy of the GNU Lesser Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//  See COPYING.txt for the full terms of the GNU Lesser Public License.
//
////////////////////////////////////////////////////////////////////////////////

#ifndef _DEBUG
#error "Visual Leak Detector requires a *debug* C runtime library (compiler option /MDd, /MLd, /MTd, or /LDd."
#endif

#pragma warning(disable:4786)       // Disable STL-induced warnings 
#pragma comment(lib, "dbghelp.lib") // Link with the Debug Help library for symbol handling and stack walking

// Standard headers
#include <cassert>
#include <map>
#include <string>
#include <vector>

// Microsoft-specific headers
#include <windows.h> // crtdbg.h, dbghelp.h, dbgint.h, and mtdll.h depend on this.
#include <crtdbg.h>  // Provides heap debugging services.
#include <dbghelp.h> // Provides stack walking and symbol handling services.
#define _CRTBLD      // Force dbgint.h and mtdll.h to allow us to include them, even though we're not building the C runtime library.
#include <dbgint.h>  // Provides access to the heap internals, specifically the memory block header structure.
#ifdef _MT
#include <mtdll.h>   // Gives us the ability to lock the heap in multithreaded builds.
#endif // _MT
#undef _CRTBLD

using namespace std;

#define VLD_VERSION "0.9d"

// Configuration Interface - these functions provide an interface to the library
// for setting configuration options at runtime. They have C linkage so that C
// programs can also configure the library.
extern "C" void VLDSetMaxDataDump (unsigned long bytes);
extern "C" void VLDSetMaxTraceFrames (unsigned long frames);
extern "C" void VLDShowUselessFrames (unsigned int show);

////////////////////////////////////////////////////////////////////////////////
//
// The VisualLeakDetector Class
//
//   One global instance of this class is instantiated. Upon construction it
//   registers our allocation hook function with the debug heap. Upon
//   destruction it checks for and reports memory leaks.
//
class VisualLeakDetector
{
    typedef vector<DWORD64> CallStack;     // Each entry in the call stack contains that frame's program counter.
    typedef map<long, CallStack> BlockMap; // Maps memory blocks to their call stacks. Keyed on the block's allocation request number.

public:
    VisualLeakDetector();
    ~VisualLeakDetector();

private:
    // Private Helper Functions - see each function definition for details.
    static int allochook (int type, void *pdata, unsigned int size, int use, long request, const unsigned char *file, int line);
    string buildsymbolsearchpath ();
#ifdef _M_IX86
    unsigned long getprogramcounterintelx86 ();
#endif // _M_IX86
    void getstacktrace (CallStack& callstack);
    void hookfree (void *pdata);
    void hookmalloc (long request);
    void hookrealloc (void *pdata);
    void reportleaks ();

    // Private Data
    BlockMap        m_mallocmap;         // Map of allocated memory blocks
    unsigned long   m_maxdatadump;       // Maximum number of bytes of user data to dump for each memory block
    unsigned long   m_maxtraceframes;    // Maximum number of stack frames to trace for each memory block
    _CRT_ALLOC_HOOK m_poldhook;          // Pointer to the previously installed allocation hook function
    HANDLE          m_process;           // Handle to the current process - required for obtaining stack traces
    unsigned int    m_showuselessframes; // Toggles display of useless stack frames (not bool for C compatibility)
#ifndef _MT
    HANDLE          m_thread;            // Handle to the current thread - required for obtaining stack traces
#else
#define m_thread GetCurrentThread()
#endif // _MT

    // Friend Functions - see each function definition for details.
    friend void VLDSetMaxDataDump (unsigned long bytes);
    friend void VLDSetMaxTraceFrames (unsigned long frames);
    friend void VLDShowUselessFrames (unsigned int show);
};

// The one and ONLY VisualLeakDetector object instance. This is placed in the
// "library" initialization area, so that it gets constructed just afer C
// runtime initialization, but before any user global objects are constructed.
// Also, disable the warning about us using the "library" initialization area.
#pragma warning(disable:4073)
#pragma init_seg(lib)
VisualLeakDetector visualleakdetector;

// Constructor - Installs our allocation hook function so that the C runtime's
//   debug heap manager will call our hook function for every heap request.
//
VisualLeakDetector::VisualLeakDetector ()
{
    static bool already_instantiated = false;

    // Disallow more than one instance of VisualLeakDetector. If this code is
    // asserting, then you've instantiated a second instance. Only one instance
    // should be created and it is already done for you (see above).
    assert(!already_instantiated);
    already_instantiated = true;

    // Register our allocation hook function with the debug heap.
    m_poldhook = _CrtSetAllocHook(allochook);

    // Initialize private data.
    m_maxdatadump       = 0xffffffff;
    m_maxtraceframes    = 0xffffffff;
    m_process           = GetCurrentProcess();
    m_showuselessframes = false;
#ifndef _MT
    m_thread = GetCurrentThread();
#endif // _MT

    _RPT0(_CRT_WARN, "Visual Leak Detector Version "VLD_VERSION" installed.\n");
}

// Destructor - Unhooks our hook function and outputs a memory leak report.
//
VisualLeakDetector::~VisualLeakDetector ()
{
    _CRT_ALLOC_HOOK pprevhook;

    // Deregister our hook function.
    pprevhook = _CrtSetAllocHook(m_poldhook);
    if (pprevhook != allochook) {
        // WTF? Somebody replaced our hook before we were done. Put theirs
        // back, but notify the human about the situation.
        _CrtSetAllocHook(pprevhook);
        _RPT0(_CRT_WARN, "Visual Leak Detector: The CRT allocation hook function was unhooked prematurely!\n"
                         "    There's a good possibility that any potential leaks have gone undetected!\n");
    }

    // Report any leaks that we find.
    reportleaks();

    _RPT0(_CRT_WARN, "Visual Leak Detector is now exiting.\n");
}

// allochook - This is a hook function that is installed into Microsoft's
//   CRT debug heap when the VisualLeakDetector object is constructed. Any time
//   an allocation, reallocation, or free is made from/to the debug heap,
//   the CRT will call into this hook function.
//
//  Note: The debug heap serializes calls to this function (i.e. the debug heap
//    is locked prior to calling this function). So we don't need to worry about
//    thread safety -- it's already taken care of for us.
//
//  - type (IN): Specifies the type of request (alloc, realloc, or free).
//
//  - pdata (IN): On a free allocation request, contains a pointer to the
//      user data section of the memory block being freed. On alloc requests,
//      this pointer will be NULL because no block has actually been allocated
//      yet.
//
//  - size (IN): Specifies the size (either real or requested) of the user
//      data section of the memory block being freed or requested. This function
//      ignores this value.
//
//  - use (IN): Specifies the "use" type of the block. This can indicate the
//      purpose of the block being requested. It can be for internal use by
//      the CRT, it can be an application defined "client" block, or it can
//      simply be a normal block. Client blocks are just normal blocks that
//      have been specifically tagged by the application so that the application
//      can separately keep track of the tagged blocks for debugging purposes.
//      Visual Leak Detector currently does not make use of the client block
//      capability because of limitations of the debug version of the delete
//      operator.
//
//  - request (IN): Specifies the allocation request number. This is basically
//      a sequence number that is incremented for each allocation request. It
//      is used to uniquely identify each allocation.
//
//  - filename (IN): String containing the filename of the source line that
//      initiated this request. This function ignores this value.
//
//  - line (IN): Line number within the source file that initiated this request.
//      This function ignores this value.
//
//  Return Value:
//
//    Always returns true, unless another allocation hook function was already
//    installed before our hook function was called, in which case we'll return
//    whatever value the other hook function returns. Returning false will
//    cause the debug heap to deny the pending allocation request (this can be
//    useful for simulating out of memory conditions, but Visual Leak Detector
//    has no need to make use of this capability).
//
int VisualLeakDetector::allochook (int type, void *pdata, unsigned int size, int use, long request, const unsigned char *file, int line)
{
    static inallochook = false;

    if (inallochook) {
        // Prevent the current thread from re-entering on allocs/reallocs/frees
        // that we do internally to record the data we are collecting.
        if (visualleakdetector.m_poldhook) {
            return (visualleakdetector.m_poldhook)(type, pdata, size, use, request, file, line);
        }
        return true;
    }

    if (use == _CRT_BLOCK) {
        // Do not attempt to keep track of blocks used internally by the CRT
        // library. Otherwise we will kick off an infinite loop when, during
        // later processing, we call into CRT functions that allocate memory.
        if (visualleakdetector.m_poldhook) {
            return (visualleakdetector.m_poldhook)(type, pdata, size, use, request, file, line);
        }
        return true;
    }

    inallochook = true;

    // Call the appropriate handler for the type of operation.
    switch (type) {
    case _HOOK_ALLOC:
        visualleakdetector.hookmalloc(request);
        break;

    case _HOOK_FREE:
        visualleakdetector.hookfree(pdata);
        break;

    case _HOOK_REALLOC:
        visualleakdetector.hookrealloc(pdata);
        break;

    default:
        _RPT1(_CRT_WARN, "Visual Leak Detector: in allochook(): Unhandled allocation type (%d).\n", type);
        break;
    }

    inallochook = false;
    if (visualleakdetector.m_poldhook) {
        return (visualleakdetector.m_poldhook)(type, pdata, size, use, request, file, line);
    }
    return true;
}

// buildsymbolsearchpath - Builds the symbol search path for the symbol handler.
//   This helps the symbol handler find the symbols for the application being
//   debugged. The default behavior of the search path doesn't appear to work
//   as documented (at least not under Visual C++ 6.0) so we need to augment the
//   default search path in order for the symbols to be found if they're in a
//   program database (PDB) file.
//
//  Return Value:
//
//    Returns a string containing a useable symbol search path to be given to
//    the symbol handler.
//
string VisualLeakDetector::buildsymbolsearchpath ()
{
    string             command = GetCommandLineA();
    char              *env;
    bool               inquote = false;
    string::size_type  length = command.length();
    string::size_type  next = 0;
    string             path;
    string::size_type  pos = 0;

    // The documentation says that executables with associated program database
    // (PDB) files have the absolute path to the PDB embedded in them and that,
    // by default, that path is used to find the PDB. That appears to not be the
    // case (at least not with Visual C++ 6.0). So we'll manually add the
    // location of the executable (which is where the PDB is located by default)
    // to the symbol search path. Use the command line to extract the path.
    //
    // Start by filtering out any command line arguments.
    while (next != length) {
        pos = command.find_first_of(" \"", next);
        if (pos == string::npos) {
            break;
        }
        if (command[pos] == '\"') {
            if (inquote) {
                inquote = false;
            }
            else {
                inquote = true;
            }
        }
        else if (command[pos] == ' ') {
            if (!inquote) {
                break;
            }
        }
        next = pos + 1;
    }
    command = command.substr(0, pos);

    // Now remove the executable file name to get just the path by itself.
    pos = command.find_last_of('\\', next);
    if ((pos == string::npos) || (pos == 0)) {
        path = "\\";
    }
    else {
        path = command.substr(0, pos);
    }

    // When the symbol handler is given a custom symbol search path, it will no
    // longer search the default directories (working directory, system root,
    // etc). But we'd like it to still search those directories, so we'll add
    // them to our custom search path.
    path += ";.\\";
    env = getenv("SYSTEMROOT");
    if (env) {
        path += string(";") + env;
        path += string(";") + env + "\\system32";
    }
    env = getenv("_NT_SYMBOL_PATH");
    if (env) {
        path += string(";") + env;
    }
    env = getenv("_NT_ALT_SYMBOL_PATH");
    if (env) {
        path += string(";") + env;
    }

    // Remove any quotes from the path. The symbol handler doesn't like them.
    pos = 0;
    while (pos != string::npos) {
        pos = path.find_first_of('\"');
        if (pos != string::npos) {
            path.erase(pos, 1);
        }
    }

    return path;
}

// getprogramcounterintelx86 - Helper function that retrieves the program
//   counter (aka the EIP register) for getstacktrace() on Intel x86
//   architecture. There is no way for software to directly read the EIP
//   register. But it's value can be obtained by calling into a function (in our
//   case, this function) and then retrieving the return address, which will be
//   the program counter from where the function was called.
//
//  Notes:
//
//    a) Frame pointer omission (FPO) optimization must be turned off so that
//       the EBP register is guaranteed to contain the frame pointer. With FPO
//       optimization turned on, EBP might hold some other value.
//
//    b) Inlining of this function must be disabled. The whole purpose of this
//       function's existence depends upon it being a *called* function.
//
//  Return Value:
//
//    Returns the return address of the current stack frame.
//
#ifdef _M_IX86
#pragma optimize ("y", off)
#pragma auto_inline(off)
unsigned long VisualLeakDetector::getprogramcounterintelx86 ()
{
    unsigned long programcounter;

    __asm mov eax, [ebp + 4]         // Get the return address out of the current stack frame
    __asm mov [programcounter], eax  // Put the return address into the variable we'll return

    return programcounter;
}
#pragma auto_inline(on)
#pragma optimize ("y", on)
#endif // _M_IX86

// getstacktrace - Traces the stack, starting from this function, as far
//   back as possible. Populates the provided CallStack with one entry for each
//   stack frame traced. Requires architecture-specific code for retrieving
//   the current frame pointer and program counter.
//
//  - callstack (OUT): Empty CallStack vector to be populated with entries from
//    the stack trace. Each frame traced will push one entry onto the CallStack.
//
//  Note:
//
//    Frame pointer omission (FPO) optimization must be turned off so that the
//    EBP register is guaranteed to contain the frame pointer. With FPO
//    optimization turned on, EBP might hold some other value.
//
//  Return Value:
//
//    None.
//
#pragma optimize ("y", off)
void VisualLeakDetector::getstacktrace (CallStack& callstack)
{
    DWORD         architecture;
    CONTEXT       context;
    unsigned int  count = 0;
    unsigned long framepointer;
    STACKFRAME64  frame;
    unsigned long programcounter;

    // Get the required values for initialization of the STACKFRAME64 structure
    // to be passed to StackWalk64(). Required fields are AddrPC and AddrFrame.
#ifdef _M_IX86
    architecture = IMAGE_FILE_MACHINE_I386;
    programcounter = getprogramcounterintelx86();
    __asm mov [framepointer], ebp  // Get the frame pointer (aka base pointer)
#else
// If you want to retarget Visual Leak Detector to another processor
// architecture then you'll need to provide architecture-specific code to
// retrieve the current frame pointer and program counter in order to initialize
// the STACKFRAME64 structure below.
#error "Visual Leak Detector is not supported on this architecture."
#endif // _M_IX86

    // Initialize the STACKFRAME64 structure.
    memset(&frame, 0x0, sizeof(frame));
    frame.AddrPC.Offset    = programcounter;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = framepointer;
    frame.AddrFrame.Mode   = AddrModeFlat;

    // Walk the stack.
    while (count < m_maxtraceframes) {
        count++;
        if (!StackWalk64(architecture, m_process, m_thread, &frame, &context,
                         NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            // Couldn't trace back through any more frames.
            break;
        }
        if (frame.AddrFrame.Offset == 0) {
            // End of stack.
            break;
        }

        // Push this frame's program counter onto the provided CallStack.
        callstack.push_back(frame.AddrPC.Offset);
    }
}
#pragma optimize ("y", on)

// hookfree - Called by the allocation hook function in response to freeing a
//   block. Removes the block (and it's call stack) from the block map.
//
//  - pdata (IN): Pointer to the user data section of the memory block being
//      freed.
//
//  Return Value:
//
//    None.
//
void VisualLeakDetector::hookfree (void *pdata)
{
    long request = pHdr(pdata)->lRequest;

    m_mallocmap.erase(request);
}

// hookmalloc - Called by the allocation hook function in response to an
//   allocation. Obtains a stack trace for the allocation and stores the
//   CallStack in the block allocation map along with the allocation request
//   number (which serves as a unique key for mapping each memory block to its
//   call stack).
//
//  - request (IN): The allocation request number. This value is provided to our
//      allocation hook function by the debug heap. We use it to uniquely
//      identify this particular allocation.
//
//  Return Value:
//
//    None.
//
void VisualLeakDetector::hookmalloc (long request)
{
    CallStack callstack;

    getstacktrace(callstack);
    m_mallocmap.insert(make_pair(request, callstack));
}

// hookrealloc - Called by the allocation hook function in response to
//   reallocating a block. The debug heap insulates us from things such as
//   reallocating a zero size block (the same as a call to free()). So we don't
//   need to check for any special cases such as that. All reallocs are
//   essentially just a free/malloc sequence.
//
//  - pdata (IN): Pointer to the user data section of the memory block being
//      reallocated.
//
//  Return Value:
//
//    None.
//
void VisualLeakDetector::hookrealloc (void *pdata)
{
    long request = pHdr(pdata)->lRequest;

    // Do a free, then do a malloc.
    hookfree(pdata);
    hookmalloc(request);
}

// reportleaks - Generates a memory leak report when the program terminates if
//   leaks were detected. The report is displayed in the debug output window.
//
//   By default, only "useful" frames are displayed in the Callstack section of
//   each memory block report. By "useful" we mean frames that are not internal
//   to the heap or Visual Leak Detector. However, if ShowUselessFrames() is
//   called with a value of "true", then all frames will be shown. If the source
//   file  information for a frame cannot be found, then the frame will be
//   displayed regardless of the state of ShowUselessFrames() (this is because
//   the useless frames are identified by the source file). In most cases, the
//   symbols for the heap internals should be available so this should rarely,
//   if ever, be a problem.
//
//   By default the entire user data section of each block is dumped following
//   the call stack. However, the data dump can be restricted to a limited
//   number of bytes by calling the SetMaxDataDump() API.
//
//  Return Value:
//
//    None.
//
void VisualLeakDetector::reportleaks ()
{
    CallStack            callstack;
    DWORD                displacement;
    DWORD64              displacement64;
    SYMBOL_INFO         *pfunctioninfo;
    string               functionname;
    CallStack::iterator  itstack;
    BlockMap::iterator   itblock;
    unsigned long        leaksfound = 0;
    string               path;
    char                *pheap;
    _CrtMemBlockHeader  *pheader;
    IMAGEHLP_LINE64      sourceinfo;
#define MAXSYMBOLNAMELENGTH 256
#define SYMBOLBUFFERSIZE (sizeof(SYMBOL_INFO) + (MAXSYMBOLNAMELENGTH * sizeof(TCHAR)) - 1)
    unsigned char        symbolbuffer [SYMBOLBUFFERSIZE];

    // Initialize structures passed to the symbol handler.
    pfunctioninfo = (SYMBOL_INFO*)symbolbuffer;
    memset(pfunctioninfo, 0x0, SYMBOLBUFFERSIZE);
    pfunctioninfo->SizeOfStruct = sizeof(SYMBOL_INFO);
    pfunctioninfo->MaxNameLen = MAXSYMBOLNAMELENGTH;
    memset(&sourceinfo, 0x0, sizeof(IMAGEHLP_LINE64));
    sourceinfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    // Initialize the symbol handler. We use it for obtaining source file/line
    // number information and function names for the memory leak report.
    path = buildsymbolsearchpath();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if (!SymInitialize(m_process, (char*)path.c_str(), TRUE)) {
        _RPT1(_CRT_WARN, "Visual Leak Detector: The symbol handler failed to initialize (error=%d).\n"
                         "    Stack traces will probably not be available for leaked blocks.\n", GetLastError());
    }

#ifdef _MT
    // If this is a multithreaded build, lock the heap while we walk the
    // allocated list -- for thread safety.
    _mlock(_HEAP_LOCK);
#endif

    // We employ a simple trick here to get a pointer to the first allocated
    // block: just allocate a new block and get the new block's memory header.
    // This works because the most recently allocated block is always placed at
    // the head of the allocated list. We can then walk the list from head to
    // tail. For each block still in the list we do a lookup to see if we have
    // an entry for that block in the allocation block map. If we do, it is a
    // leaked block and the map entry contains the call stack for that block.
    pheap = new char;
    pheader = pHdr(pheap)->pBlockHeaderNext;
    while (pheader) {
        itblock = m_mallocmap.find(pheader->lRequest);
        if (itblock != m_mallocmap.end()) {
            // Found a block which is still in the allocated list, and which we
            // have an entry for in the allocated block map. We've identified a
            // memory leak.
            if (leaksfound == 0) {
                _RPT0(_CRT_WARN, "Detected memory leaks!\n");
            }
            leaksfound++;
            _RPT3(_CRT_WARN, "---------- Block %d at 0x%08X: %d bytes ----------\n", pheader->lRequest, pbData(pheader), pheader->nDataSize);
            _RPT0(_CRT_WARN, "  Call Stack:\n");

            // Iterate through each frame in the call stack.
            callstack = (*itblock).second;
            for (itstack = callstack.begin(); itstack != callstack.end(); itstack++) {
                // Try to get the source file and line number associated with
                // this program counter address.
                if (SymGetLineFromAddr64(m_process, *itstack, &displacement, &sourceinfo)) {
                    // Unless m_showuselessframes has been toggled, don't show
                    // frames that are internal to the heap or Visual Leak
                    // Detector. There is virtually no situation where they
                    // would be useful for finding the source of the leak.
                    if (!m_showuselessframes) {
                        if (strstr(sourceinfo.FileName, "afxmem.cpp") ||
                            strstr(sourceinfo.FileName, "dbgheap.c") ||
                            strstr(sourceinfo.FileName, "new.cpp") ||
                            strstr(sourceinfo.FileName, "vld.cpp")) {
                            continue;
                        }
                    }
                }

                // Try to get the name of the function containing this program
                // counter address.
                if (SymFromAddr(m_process, *itstack, &displacement64, pfunctioninfo)) {
                    functionname = pfunctioninfo->Name;
                }
                else {
                    functionname = "(Function name unavailable)";
                }

                // Display the current stack frame's information.
                if (sourceinfo.FileName) {
                    _RPT3(_CRT_WARN, "    %s (%d): %s\n", sourceinfo.FileName, sourceinfo.LineNumber, functionname.c_str());
                }
                else {
                    _RPT1(_CRT_WARN, "    0x%08X (File and line number not available): ", *itstack);
                    _RPT1(_CRT_WARN, "%s\n", functionname.c_str());
                }
            }

            // Dump the data in the user data section of the memory block.
            string        ascdump;
            unsigned int  byte;
            unsigned int  bytesdone;
            unsigned int  datalen;
            unsigned char datum;
            unsigned int  dumplen;
            char          formatbuf [4];
            string        hexdump;

            if (m_maxdatadump == 0) {
                continue;
            }
            datalen = (m_maxdatadump < pheader->nDataSize) ? m_maxdatadump : pheader->nDataSize;
            // Each line of output is 16 bytes.
            if ((datalen % 16) == 0) {
                // No padding needed.
                dumplen = datalen;
            }
            else {
                // We'll need to pad the last line out to 16 bytes.
                dumplen = datalen + (16 - (datalen % 16));
            }
            // For each byte of data, get both the ASCII equivalent (if it is a
            // printable character) and the hex representation.
            _RPT0(_CRT_WARN, "  Data:\n");
            bytesdone = 0;
            for (byte = 0; byte < dumplen; byte++) {
                if (byte < datalen) {
                    datum = ((unsigned char*)pbData(pheader))[byte];
                    sprintf(formatbuf, "%.2X ", datum);
                    hexdump += formatbuf;
                    if (isprint(datum) && (datum != ' ')) {
                        ascdump += datum;
                    }
                    else {
                        ascdump += '.';
                    }
                }
                else {
                    // Add padding to fill out the last line to 16 bytes.
                    hexdump += "   ";
                    ascdump += '.';
                }
                bytesdone++;
                if ((bytesdone % 16) == 0) {
                    // Print one line of data for every 16 bytes. Include the
                    // ASCII dump and the hex dump side-by-side.
                    _RPT2(_CRT_WARN, "    %s    %s\n", hexdump.c_str(), ascdump.c_str());
                    hexdump = "";
                    ascdump = "";
                }
                else {
                    if ((bytesdone % 8) == 0) {
                        // Add a spacer in the ASCII dump after every two words.
                        ascdump += " ";
                    }
                    if ((bytesdone % 4) == 0) {
                        // Add a spacer in the hex dump after every word.
                        hexdump += "  ";
                    }
                }
            }
            _RPT0(_CRT_WARN, "\n");
        }
        pheader = pheader->pBlockHeaderNext;
    }
#ifdef _MT
    // Unlock the heap if this is a multithreaded build.
    _munlock(_HEAP_LOCK);
#endif
    delete pheap;
    if (!leaksfound) {
        _RPT0(_CRT_WARN, "No memory leaks detected.\n");
    }
    else {
        _RPT1(_CRT_WARN, "Detected %d memory leak", leaksfound);
        _RPT0(_CRT_WARN, (leaksfound > 1) ? "s.\n" : ".\n");
    }

    if (!SymCleanup(m_process)) {
        _RPT1(_CRT_WARN, "Visual Leak Detector: The symbol handler failed to deallocate resources (error=%d).\n", GetLastError());
    }
}

////////////////////////////////////////////////////////////////////////////////
//
//  Configuration Interface - Friend Functions of the VisualLeakDetector Class
//
//    These functions exist to allow the Visual Leak Detector configurator
//    (which is external to the Visual Leak Detector library) to configure
//    the Visual Leak Detector at runtime. The configurator itself is built
//    at application compile-time and depends on the optional preprocessor
//    macros VLD_MAX_DATA_DUMP, VLD_MAX_TRACE_FRAMES, and
//    VLD_SHOW_USELESS_FRAMES.
//

// VLDSetMaxDataDump - Sets the maximum number of bytes of user data to be
//   dumped in memory leak reports. By default, the entire contents of the user
//   data section of each memory block is included in the dump.
//
//  - bytes (IN): Specifies the maximum number of bytes to include in the dump.
//      Set to zero to suppress data dumps altogether.
//
//  Return Value:
//
//    None.
//
void VLDSetMaxDataDump (unsigned long bytes)
{
    visualleakdetector.m_maxdatadump = bytes;
}

// VLDSetMaxTraceFrames - Sets the maximum number of stack frames to trace for
//   each allocated memory block. By default, the stack is traced as far back as
//   possible for each block. However, this can add considerable overhead
//   in both CPU utilization and memory utilization. Limiting the stack trace
//   to a preset maximum can significantly decrease this overhead.
//
//  Note: Within the context of this function, the specified frame count
//    includes the "useless" frames (see ShowUselessFrames()) which, by default,
//    are not shown in the memory leak report. Keep this in mind when using this
//    function or you may not see the number of frames that you expect.
//
//  - frames (IN): Specifies the maximum number of frames to trace.
//
//  Return Value:
//
//    None.
//
void VLDSetMaxTraceFrames (unsigned long frames)
{
    visualleakdetector.m_maxtraceframes = frames;
}

////////////////////////////////////////////////////////////////////////////////
// VLDShowUselessFrames - Toggles the optional display of "useless" frames.
//   Useless frames are those that are internal to the heap or Visual Leak
//   Detector. By default, display of these useless frames is suppressed in the
//   memory leak report.
//
//  - show (IN): Boolean value which either enables or disables display of
//      useless frames in the memory leak report. If nonzero, useless frames
//      will be displayed. If zero, display of useless frames will be suppressed
//      (this is the default).
//
//  Return Value:
//
//    None.
//
void VLDShowUselessFrames (unsigned int show)
{
    visualleakdetector.m_showuselessframes = show;
}