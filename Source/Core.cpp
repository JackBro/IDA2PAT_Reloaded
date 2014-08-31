
// ****************************************************************************
// File: Core.cpp
// Desc: IDA2PAT Reloaded plug-in by Sirmabus
//
// ****************************************************************************
#include "stdafx.h"
using namespace std;

typedef map<ea_t, ea_t, less<ea_t>> REFMAP;

// Minimum function size to consider
static const UINT MIN_FUNC_SIZE = 8; 


// === Function Prototypes ===
static BOOL CheckBreak();
static BOOL ProcessFuncion(func_t *pFunc);
static void MakeSig(ea_t startEA, ea_t endEA);
static LPCTSTR TimeString(TIMESTAMP Time);

// === Data ===
static UINT s_uPatterns = 0;
static FILE *s_fp = NULL;


// Initialize
void CORE_Init()
{
	s_uPatterns = 0;
}

// Un-initialize
void CORE_Exit()
{
}


// Plug-in process
void CORE_Process(int iArg)
{
	msg("\n== IDA2PAT-Reloaded plug-in: v: %s - %s, By Sirmabus ==\n", MY_VERSION, __DATE__);	

	if(autoIsOk())
	{				
		// Ask for pattern file name
		LPSTR pszFileName = NULL;
		while(TRUE)
		{
			if(pszFileName = askfile_c(1, "*.pat", "<IDS2PAT-Reloaded>:  Enter the save name for the pattern file:"))
			{
				if(PathFileExistsA(pszFileName))
				{
					int iResult = askyn_c(-1, "Do you want to overwrite the existing file?");					
					if(iResult == 0)
						continue;
					else
					if(iResult == -1)
						pszFileName = NULL; 					
				}
			}
			
			break;
		};

		if(!pszFileName)
		{
			msg(" - Canceled -\n");
			hide_wait_box();
			return;
		}

		// Open it
		if(s_fp = qfopen(pszFileName, "w+b"))
		{	
			msg("Functions to process: %d\n\n", get_func_qty());

			msg("Working, <Press Pause/Break key to abort>...\n");		
			show_wait_box("Working..\nTake a smoke, drink some coffee, this could be a while..  \n\n<Press Pause/Break key to abort>");
					
			// Iterate through all functions..
			TIMESTAMP StartTime = GetTimeStamp();
			int iFuncCount = get_func_qty();
			for(int iIndex = 0; iIndex < iFuncCount; iIndex++)
			{
				if(func_t *pFunc = getn_func(iIndex))
				{			
					// Process it..
					if(!ProcessFuncion(pFunc))
						break;
			
					// Bail out on cancel request
					if(CheckBreak())
						break;		
				}
				else
				{
					msg("\n*** Failed to get function for index: %d! ***\n", iIndex);					
					s_uPatterns = 0;
					break;
				}
			}

			msg("Patterns created: %u\n", s_uPatterns);			
			msg("    Process time: %s.\n", TimeString(GetTimeStamp() - StartTime));
			msg("Finished.\n-------------------------------------------------------------\n");

			// EOF marker and close
			qfprintf(s_fp, "---\n");
			qfclose(s_fp);
			s_fp = NULL;
			hide_wait_box();
		}
		else
		{
			warning("Failed to create pattern file!");
			msg("\n*** Aborted ***\n");
		}
	}
	else
	{
		warning("Autoanalysis must finish first before you run this plug-in!");
		msg("\n*** Aborted ***\n");
	}
}

// Checks and handles if break key pressed; returns TRUE on break.
static BOOL CheckBreak()
{
	if(wasBreak() || (GetAsyncKeyState(VK_PAUSE) & 0x8000))
	{			
		msg("\n*** Aborted ***\n\n");	
		return(TRUE);
	}

	return(FALSE);
}


// Return TRUE if we should use a function reference by name
static ALIGN(32) BOOL IsNameWanted(LPSTR pszName)
{
	// Function of interest?
	// not "sub_.." or "unknown_libname_.."
	if(
		// not "sub_..
		/*"sub_"*/ (*((PUINT) pszName) != 0x5F627573) &&

		// not "unknown_libname_..
		/*"unknown_"*/ ((*((PUINT64) pszName) != 0x5F6E776F6E6B6E75) && (*((PUINT64) (pszName + 8)) != /*"libname_"*/ 0x5F656D616E62696C)) &&

		// not nullsub_..
		/*"nullsub_"*/ (*((PUINT64) pszName) != 0x5F6275736C6C756E)	&&
	
		// not "SEH_..
		/*"SEH_"*/ (*((PUINT) pszName) != 0x5F484553)
	  )
	{	
		// Skip common auto-generated demangled import names,etc.		
		// ?, @, $, _
		if(!((pszName[0] == '?') || (pszName[0] == '@') || (pszName[0] == '$') || (pszName[0] == '_')))
		{
			// Skip auto-generated "thunk" stubs			
			int iLen = strlen(pszName);
			if(iLen > SIZESTR("_thunk"))			
				if(strcmp((pszName + (iLen - SIZESTR("_thunk"))), "_thunk") == 0)
					return(FALSE);
	
			// Skip common entry point names
			if((strcmp(pszName, "start") == 0) || (strcmp(pszName, "StartAddress") == 0))
				return(FALSE);

			return(TRUE);				
		}
	}

	return(FALSE);
}


// Process a function
static ALIGN(32) BOOL ProcessFuncion(func_t *pFunc)
{
	// Reject small functions	
	if(pFunc->size() >= MIN_FUNC_SIZE)
	{	
		// Get name
		char szName[MAXNAMELEN + 1];
		if(get_true_name(BADADDR, pFunc->startEA, szName, SIZESTR(szName)))
		{
			// Testing stuff.. these just won't work consistently :-/
			//if(pFunc->flags & FUNC_LIB)				
			//if(getFlags(pFunc->startEA) & FUNC_LIB)				
			//if(is_public_name(pFunc->startEA)) // ??6@YAAAVDbRowData_c@@AAV0@_N@Z			
			//if(dummy_name_ea(pszName) == BADADDR)
			//	msg("%08X D: \"%s\".\n", pFunc->startEA, pszName);

			// Filter function by name pattern
			if(IsNameWanted(szName))
			{
				// Create a pattern for this function
				MakeSig(pFunc->startEA, pFunc->endEA);
				s_uPatterns++;			
			}
		}	
	}	

	return(TRUE);
}


// ****************************************************************************
// Func: FindRefLoc()
// Desc: This function finds the location of a reference within an instruction
//		 or a data item e.g.
//       .text:00401000  E8 FB 0F 00 00   call sub_402000
//
//		 FindRefLoc(0x401000, 0x402000) would return 0x401001
//	     it works for both segment relative and self-relative offsets
//
// Note: All references are assumed to be 4 bytes long.
//       This is will be problematic for some processors.
//
// ****************************************************************************
inline ea_t FindRefLoc(ea_t item, ea_t _ref)
{
	if(isCode(getFlags(item)))
	{
		ua_ana0(item);
		if(cmd.Operands[0].type == o_near)
		{
			// We have got a self-relative reference
			_ref = (_ref - get_item_end(item));
		}
	}

	for(ea_t i = item; i <= (get_item_end(item) - 4); i++)
	{
		if(_ref == get_long(i))		
			return(i);		
	}

	return(BADADDR);
}

// Mark off a string of bytes as variable
inline void SetVBytes(vector<bool> &bv, UINT pos)
{ 	
	bv[pos + 0] = true;
	bv[pos + 1] = true;	
	bv[pos + 2] = true;	
	bv[pos + 3] = true;	
}


// Create signature from function and add it to pattern list
static void MakeSig(ea_t startEA, ea_t endEA)
{
	ea_t Length = (endEA - startEA);
	ALIGN(16) vector<bool> v_bytes(Length);
	ALIGN(16) vector<ea_t> v_publics;
	ALIGN(16) REFMAP refs;

	// PART #1  Get the pubs and references
	ea_t ea = startEA;
	while((ea != BADADDR) && (ea < endEA))
	{		
		// Load up the publics vector
		// Does the current byte have non-trivial (non-dummy) name?
		flags_t flags = getFlags(ea);
		if(has_name(flags))
		{
			// Filter out auto-generated names that pass through
			char szName[MAXNAMELEN + 1];
			if(get_true_name(BADADDR, ea, szName, SIZESTR(szName)))
			{
				if(IsNameWanted(szName))
					v_publics.push_back(ea);
			}			
		}

		// Load up references map
		ea_t ref = get_first_dref_from(ea);
		if(ref != BADADDR)
		{
			// A data location is referenced
			ea_t ref_loc = FindRefLoc(ea, ref);
			if(ref_loc != BADADDR)
			{
				SetVBytes(v_bytes, (ref_loc - startEA));
				refs[ref_loc] = ref;
			}

			// Check if there is a second data location ref'd			
			if((ref = get_next_dref_from(ea, ref)) != BADADDR)
			{				
				if((ref_loc = FindRefLoc(ea, ref)) != BADADDR)
				{
					SetVBytes(v_bytes, (ref_loc - startEA));
					refs[ref_loc] = ref;
				}
			}
		}
		else
		{
			// Do we have a code ref?			
			if((ref = get_first_fcref_from(ea)) != BADADDR)
			{
				// If so, make sure it is outside of function
				if((ref < startEA) || (ref >= (startEA + Length)))
				{	
					ea_t ref_loc = FindRefLoc(ea, ref);
					if(ref_loc != BADADDR)
					{
						SetVBytes(v_bytes, ref_loc - startEA);
						refs[ref_loc] = ref;
					}
				}
			}
		}

		ea = next_not_tail(ea);
	};
	

	// PART #2
	// Write out the first string of bytes making sure not to go past the end of the function
	UINT uFirstString = ((Length < 32) ? Length : 32);
	for(UINT i = 0; i < uFirstString; i++)
	{
		if(v_bytes[i])		
			qfprintf(s_fp, "..");		
		else		
			qfprintf(s_fp, "%02X", get_byte(startEA + i));
	}

	// PART #3
	// Fill in anything less than 32
	for(UINT i = 0; i < (32 - uFirstString); i++)	
		qfprintf(s_fp, "..");	

	// PART #4
	// Put together the CRC data
	BYTE aCRC[256] = {0};
	UINT pos = 32;
	while((pos < Length) && (!v_bytes[pos]) && (pos < (255 + 32)))
	{
		aCRC[(pos - 32)] = get_byte(startEA + pos);
		pos++;
	};

	// PART #5
	// alen is length of the CRC data
	UINT alen = (pos - 32);
	WORD wCRC = GetCRC16(aCRC, alen);
	qfprintf(s_fp, " %02X %04X %04X", alen, wCRC, Length);


	// PART #6:    Write Public Names
	// write the publics
	for(std::vector<ea_t>::iterator p = v_publics.begin(); p != v_publics.end(); p++)
	{
		// Get name of public
		char szName[MAXNAMELEN + 1];
		if(get_true_name(BADADDR, *p, szName, MAXNAMELEN))
		{
			int xoff = (*p - startEA);
		
			// Check for negative offset and adjust output
			if(xoff >= 0)		
				qfprintf(s_fp, " :%04X %s", xoff, szName);		
			else						
				qfprintf(s_fp, " :-%04X %s", -xoff, szName);					
		}
	}

	// PART #7     Write named references
	// (*r).first   holds the ea in the function where the reference is used
	// (*r).second  holds the ea of the reference itself
	// write the references
	for(REFMAP::iterator r = refs.begin(); r != refs.end(); r++)
	{
		// Get name of reference
		char szName[MAXNAMELEN + 1];
		if(get_true_name(BADADDR, (*r).second, szName, MAXNAMELEN))
		{
			int xoff = ((*r).first - startEA);
			flags_t mflags = getFlags((*r).second);

			// Check for negative offset and adjust output
			if(xoff >= 0)			
				qfprintf(s_fp, " ^%04X %s", xoff, szName);			
			else
				qfprintf(s_fp, " ^-%04X %s", -xoff, szName);			
		}		
	}

	// PART #8
	// And finally write out the last string with the rest of the function
	qfprintf(s_fp, " ");
	for(int i = pos; i < (int) Length; i++)
	{
		if(v_bytes[i])		
			qfprintf(s_fp, "..");	
		else 		
			qfprintf(s_fp, "%02X", get_byte(startEA + i));		
	}

	qfprintf(s_fp, "\n");
}


// Make a pretty delta time string for output
static LPCTSTR TimeString(TIMESTAMP Time)
{
	static char szBuff[64];
	ZeroMemory(szBuff, sizeof(szBuff));

	if(Time >= HOUR)  
		_snprintf(szBuff, (sizeof(szBuff) - 1), "%.2f hours", (Time / (TIMESTAMP) HOUR));    
	else
	if(Time >= MINUTE)    
		_snprintf(szBuff, (sizeof(szBuff) - 1), "%.2f minutes", (Time / (TIMESTAMP) MINUTE));    
	else
		_snprintf(szBuff, (sizeof(szBuff) - 1), "%.2f seconds", Time);    

	return(szBuff);
}

