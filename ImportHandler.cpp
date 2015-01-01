#include <windows.h>
#include <fstream>
#include "ImportHandler.h"

#include "HunkList.h"
#include "Hunk.h"
#include "StringMisc.h"
#include "Log.h"
#include "Symbol.h"

#include <algorithm>
#include <vector>
#include <set>
#include <iostream>
#include <ppl.h>

using namespace std;

char *LoadDLL(const char *name) {
	char* module = (char *)((int)LoadLibraryEx(name, 0, DONT_RESOLVE_DLL_REFERENCES) & -4096);
	if(module == 0) {
		Log::error("", "Cannot open DLL '%s'", name);
	}
	return module;
}

int getOrdinal(const char* function, const char* dll) {
	char* module = LoadDLL(dll);

	IMAGE_DOS_HEADER* dh = (IMAGE_DOS_HEADER*)module;
	IMAGE_FILE_HEADER* coffHeader = (IMAGE_FILE_HEADER*)(module + dh->e_lfanew+4);
	IMAGE_OPTIONAL_HEADER32* pe = (IMAGE_OPTIONAL_HEADER32*)(coffHeader+1);
	IMAGE_EXPORT_DIRECTORY* exportdir = (IMAGE_EXPORT_DIRECTORY*) (module + pe->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
	
	short* ordinalTable = (short*) (module + exportdir->AddressOfNameOrdinals);
	int* nameTable = (int*)(module + exportdir->AddressOfNames);
	for(int i = 0; i < (int)exportdir->NumberOfNames; i++) {
		int ordinal = ordinalTable[i] + exportdir->Base;
		char* name = module+nameTable[i];
		if(strcmp(name, function) == 0) {
			return ordinal;
		}
	}

	Log::error("", "Import '%s' cannot be found in '%s'", function, dll);
	return -1;
}

char *getForwardRVA(const char* dll, const char* function) {
	char* module = LoadDLL(dll);
	IMAGE_DOS_HEADER* pDH = (PIMAGE_DOS_HEADER)module;
	IMAGE_NT_HEADERS* pNTH = (PIMAGE_NT_HEADERS)(module + pDH->e_lfanew);
	IMAGE_EXPORT_DIRECTORY* pIED = (PIMAGE_EXPORT_DIRECTORY)(module + pNTH->OptionalHeader.DataDirectory[0].VirtualAddress);

	short* ordinalTable = (short*)(module + pIED->AddressOfNameOrdinals);
	DWORD* namePointerTable = (DWORD*)(module + pIED->AddressOfNames);
	DWORD* addressTableRVAOffset = addressTableRVAOffset = (DWORD*)(module + pIED->AddressOfFunctions);

	for(unsigned int i = 0; i < pIED->NumberOfNames; i++) {
		short ordinal = ordinalTable[i];
		char* name = (char*)(module + namePointerTable[i]);

		if(strcmp(name, function) == 0) {
			DWORD address = addressTableRVAOffset[ordinal];
			if(address >= pNTH->OptionalHeader.DataDirectory[0].VirtualAddress &&
				address < pNTH->OptionalHeader.DataDirectory[0].VirtualAddress + pNTH->OptionalHeader.DataDirectory[0].Size)
				return module + address;
			return NULL;
		}
	}

	Log::error("", "Import '%s' cannot be found in '%s'", function, dll);
	return false;
}


bool importHunkRelation(const Hunk* h1, const Hunk* h2) {
	//sort by dll name
	if(strcmp(h1->getImportDll(), h2->getImportDll()) != 0) {
		//kernel32 always first
		if(strcmp(h1->getImportDll(), "kernel32") == 0)
			return true;
		if(strcmp(h2->getImportDll(), "kernel32") == 0)
			return false;

		//then user32, to ensure MessageBoxA@16 is ready when we need it
		if(strcmp(h1->getImportDll(), "user32") == 0)
			return true;
		if(strcmp(h2->getImportDll(), "user32") == 0)
			return false;


		return strcmp(h1->getImportDll(), h2->getImportDll()) < 0;
	}

	//sort by ordinal
	return getOrdinal(h1->getImportName(), h1->getImportDll()) < 
		getOrdinal(h2->getImportName(), h2->getImportDll());
}

const int hashCode(const char* str) {
	int code = 0;
	char eax;
	do {
		code = _rotl(code, 6);
		eax = *str++;
		code ^= eax;

	} while(eax);
	return code;
}


HunkList* ImportHandler::createImportHunks(HunkList* hunklist, Hunk*& hashHunk, const vector<string>& rangeDlls, bool verbose, bool& enableRangeImport) {
	if(verbose)
		printf("\n-Imports----------------------------------\n");

	vector<Hunk*> importHunks;
	vector<bool> usedRangeDlls(rangeDlls.size());

	//fill list for import hunks
	enableRangeImport = false;
	for(int i = 0; i <hunklist->getNumHunks(); i++) {
		Hunk* hunk = (*hunklist)[i];
		if(hunk->getFlags() & HUNK_IS_IMPORT) {
			do {
				char *forward = getForwardRVA(hunk->getImportDll(), hunk->getImportName());
				if (forward == NULL) break;

				string dllName, functionName;
				int sep = strstr(forward, ".")-forward;
				dllName.append(forward, sep);
				dllName = toLower(dllName);
				functionName.append(&forward[sep+1], strlen(forward)-(sep+1));
				Log::warning("", "Import '%s' from '%s' uses forwarded RVA. Replaced by '%s' from '%s'", 
					hunk->getImportName(), hunk->getImportDll(), functionName.c_str(), dllName.c_str());
				hunk = new Hunk(hunk->getName(), functionName.c_str(), dllName.c_str());
			} while (true);

			//is the dll a range dll?
			for(int i = 0; i < (int)rangeDlls.size(); i++) {
				if(toUpper(rangeDlls[i]) == toUpper(hunk->getImportDll())) {
					usedRangeDlls[i] = true;
					enableRangeImport = true;
					break;
				}
			}
			importHunks.push_back(hunk);
		}
	}

	//warn about unused range dlls
	{
		for(int i = 0; i < (int)rangeDlls.size(); i++) {
			if(!usedRangeDlls[i]) {
				Log::warning("", "No functions were imported from range dll '%s'", rangeDlls[i].c_str());
			}
		}
	}

	//sort import hunks
	sort(importHunks.begin(), importHunks.end(), importHunkRelation);

	vector<unsigned int> hashes;
	Hunk* importList = new Hunk("ImportListHunk", 0, HUNK_IS_WRITEABLE, 16, 0, 0);
	char dllNames[1024] = {0};
	char* dllNamesPtr = dllNames+1;
	char* hashCounter = dllNames;
	string currentDllName;
	int pos = 0;
	for(vector<Hunk*>::const_iterator it = importHunks.begin(); it != importHunks.end();) {
		Hunk* importHunk = *it;
		bool useRange = false;

		//is the dll a range dll?
		for(int i = 0; i < (int)rangeDlls.size(); i++) {
			if(toUpper(rangeDlls[i]) == toUpper(importHunk->getImportDll())) {
				usedRangeDlls[i] = true;
				useRange = true;
				break;
			}
		}

		//skip non hashes
		if(currentDllName.compare(importHunk->getImportDll()))
		{
			if(strcmp(importHunk->getImportDll(), "kernel32") != 0)
			{
				strcpy_s(dllNamesPtr, sizeof(dllNames)-(dllNamesPtr-dllNames), importHunk->getImportDll());
				dllNamesPtr += strlen(importHunk->getImportDll()) + 2;
				hashCounter = dllNamesPtr-1;
				*hashCounter = 0;
			}


			currentDllName = importHunk->getImportDll();
			if(verbose)
				printf("%s\n", currentDllName.c_str());
		}

		(*hashCounter)++;
		int hashcode = hashCode(importHunk->getImportName());
		hashes.push_back(hashcode);
		int startOrdinal = getOrdinal(importHunk->getImportName(), importHunk->getImportDll());
		int ordinal = startOrdinal;

		//add import
		if(verbose) {
			if(useRange)
				printf("  ordinal range {\n  ");
			printf("  %s (ordinal %d, hash %08X)\n", (*it)->getImportName(), startOrdinal, hashcode);
		}

		importList->addSymbol(new Symbol(importHunk->getName(), pos*4, SYMBOL_IS_RELOCATEABLE, importList));
		it++;

		while(useRange && it != importHunks.end() && currentDllName.compare((*it)->getImportDll()) == 0)	// import the rest of the range
		{
			int o = getOrdinal((*it)->getImportName(), (*it)->getImportDll());
			if(o - startOrdinal >= 254)
				break;

			if(verbose) {
				printf("    %s (ordinal %d)\n", (*it)->getImportName(), o);
			}

			ordinal = o;
			importList->addSymbol(new Symbol((*it)->getName(), (pos+ordinal-startOrdinal)*4, SYMBOL_IS_RELOCATEABLE, importList));
			it++;
		}

		if(verbose && useRange)
			printf("  }\n");

		if(enableRangeImport)
			*dllNamesPtr++ = ordinal - startOrdinal + 1;
		pos += ordinal - startOrdinal + 1;
	}
	*dllNamesPtr++ = -1;
	importList->setVirtualSize(pos*4);
	importList->addSymbol(new Symbol("_ImportList", 0, SYMBOL_IS_RELOCATEABLE, importList));
	importList->addSymbol(new Symbol(".bss", 0, SYMBOL_IS_RELOCATEABLE|SYMBOL_IS_SECTION, importList, "crinkler import"));

	hashHunk = new Hunk("HashHunk", (char*)&hashes[0], 0, 0, hashes.size()*sizeof(unsigned int), hashes.size()*sizeof(unsigned int));
	
	//create new hunklist
	HunkList* newHunks = new HunkList;

	newHunks->addHunkBack(importList);

	Hunk* dllNamesHunk = new Hunk("DllNames", dllNames, HUNK_IS_WRITEABLE, 0, dllNamesPtr - dllNames, dllNamesPtr - dllNames);
	dllNamesHunk->addSymbol(new Symbol(".data", 0, SYMBOL_IS_RELOCATEABLE|SYMBOL_IS_SECTION, dllNamesHunk, "crinkler import"));
	dllNamesHunk->addSymbol(new Symbol("_DLLNames", 0, SYMBOL_IS_RELOCATEABLE, dllNamesHunk));
	newHunks->addHunkBack(dllNamesHunk);

	return newHunks;
}

__forceinline unsigned int hashCode_1k(const char* str, int family, int hash_bits)
{
	int eax = 0;
	unsigned char c;
	do
	{
		c = *str++;
		eax = ((eax & 0xFFFFFF00) + c) * family;
	} while(c & 0x7F);

	eax = (eax & 0xFFFFFF00) | (unsigned char)(c + c);

	return ((unsigned int)eax) >> (32 - hash_bits);
}

static bool solve_constraints(std::vector<unsigned int>& constraints, unsigned int* new_order)
{
	if(constraints[0] > 1)	// kernel32 must be first. it can't have dependencies on anything else
	{
		return false;
	}

	std::vector<unsigned int> constraints2 = constraints;
	unsigned int used_mask = 0;

	int num = constraints.size();
	for(int i = 0; i < num; i++)
	{
		int selected = -1;
		for(int j = 0; j < num; j++)
		{
			if(((used_mask >> j) & 1) == 0 && (constraints[j] == 0))
			{
				selected = j;
				break;
			}
		}

		if(selected == -1)
		{
			return false;
		}

		
		*new_order++ = selected;
		used_mask |= (1u<<selected);
		for(int j = 0; j < num; j++)
		{
			constraints[j] &= ~(1u<<selected);
		}
	}

	return true;
}

static bool findCollisionFreeHashFamily(vector<string>& dlls, const vector<Hunk*>& importHunks, int& hash_family, int& hash_bits)
{
	int stime = GetTickCount();
	printf("searching for hash function:\n"); fflush(stdout);

	assert(dlls.size() <= 32);

	//TODO: fix this!
	dlls.erase(std::find(dlls.begin(), dlls.end(), string("kernel32")));
	dlls.insert(dlls.begin(), "kernel32");
	

	struct SDllInfo
	{
		char*				module;
		bool				allow_self_collision;
		std::vector<char>	used;
	};

	int num_dlls = dlls.size();
	std::vector<unsigned int> best_dll_order(num_dlls);

	// load dlls and mark functions that are imported
	vector<SDllInfo> dllinfos(dlls.size());
	
	for(int dll_index = 0; dll_index < num_dlls; dll_index++)
	{
		const char* dllname = dlls[dll_index].c_str();
		SDllInfo& info = dllinfos[dll_index];
		
		char* module = LoadDLL(dllname);
		info.module = module;

		IMAGE_DOS_HEADER* dh = (IMAGE_DOS_HEADER*)module;
		IMAGE_FILE_HEADER* coffHeader = (IMAGE_FILE_HEADER*)(module + dh->e_lfanew+4);
		IMAGE_OPTIONAL_HEADER32* pe = (IMAGE_OPTIONAL_HEADER32*)(coffHeader + 1);
		IMAGE_EXPORT_DIRECTORY* exportdir = (IMAGE_EXPORT_DIRECTORY*)(module + pe->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
		int num_names = exportdir->NumberOfNames;
		int* nameTable = (int*)(module + exportdir->AddressOfNames);
		info.used.resize(num_names);
		info.allow_self_collision = strcmp(dllname, "opengl32") == 0 || (strlen(dllname) == 8 && memcmp(dllname, "d3dx9_", 6) == 0);	//d3dx9_43

		for(Hunk* importHunk : importHunks)
		{
			if(strcmp(dllname, importHunk->getImportDll()) == 0)
			{
				int idx = -1;
				for(int i = 0; i < num_names; i++)
				{
					const char* name = module + nameTable[i];
					if(strcmp(name, importHunk->getImportName()) == 0)
					{
						info.used[i] = true;
						idx = i;
						break;
					}
				}

				assert(idx != -1);
			}
		}
	}

	const int MAX_BITS = 16;

	int best_num_bits = INT_MAX;
	
	// Find hash function that works
	// For future compatibility we don't allow functions from the same dlls to hash to the same value, even if the one we are interested in ends on top.
	// We do however allow hash overlaps from separate dlls.
	// To exploit this we sort the dlls to avoid collisions when possible
	
	struct SBucket
	{
		unsigned int	unreferenced_functions_dll_mask;
		unsigned char	referenced_function_dll_index;		//dll_index + 1
	};
	int best_low_byte = INT_MAX;
	int best_high_byte = INT_MAX;
	
	concurrency::critical_section cs;
	for(int num_bits = MAX_BITS; num_bits >= 1; num_bits--)
	{
		concurrency::parallel_for(0, 256, [&](int high_byte)	//TODO: don't start from 0
		{
			{
				Concurrency::critical_section::scoped_lock l(cs);
				if(num_bits == best_num_bits && high_byte > best_high_byte)
				{
					return;
				}
			}
			std::vector<unsigned int> dll_constraints(num_dlls);
			std::vector<unsigned int> new_dll_order(num_dlls);
			SBucket* buckets = new SBucket[1 << num_bits];
			for(int low_byte = 0; low_byte < 256; low_byte++)
			{
				for(int dll_index = 0; dll_index < num_dlls; dll_index++)
				{
					dll_constraints[dll_index] = 0;
				}

				int family = (high_byte<<16) | (low_byte<<8) | 1;

				memset(buckets, 0, sizeof(SBucket) << num_bits);
				bool has_collisions = false;

				unsigned int dll_index = 0;
				for(SDllInfo& dllinfo : dllinfos)
				{
					unsigned int dll_mask = (1u << dll_index);
					char* module = dllinfo.module;

					IMAGE_DOS_HEADER* dh = (IMAGE_DOS_HEADER*)module;
					IMAGE_FILE_HEADER* coffHeader = (IMAGE_FILE_HEADER*)(module + dh->e_lfanew+4);
					IMAGE_OPTIONAL_HEADER32* pe = (IMAGE_OPTIONAL_HEADER32*)(coffHeader + 1);
					IMAGE_EXPORT_DIRECTORY* exportdir = (IMAGE_EXPORT_DIRECTORY*)(module + pe->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
					int num_names = (int)exportdir->NumberOfNames;
					int* nameTable = (int*)(module + exportdir->AddressOfNames);
					for(int i = 0; i < num_names; i++)
					{
						unsigned int hashcode = hashCode_1k(module + nameTable[i], family, num_bits);
						bool new_referenced = dllinfo.used[i];
						bool old_referenced = buckets[hashcode].referenced_function_dll_index > 0;

						if(new_referenced)
						{
							if(old_referenced)
							{
								has_collisions = true;
								break;
							}
							else
							{
								buckets[hashcode].referenced_function_dll_index = dll_index + 1;
								if(dllinfo.allow_self_collision)
								{
									buckets[hashcode].unreferenced_functions_dll_mask &= ~dll_mask;	// clear unreferenced before this
								}
								else
								{
									if(buckets[hashcode].unreferenced_functions_dll_mask & dll_mask)
									{
										has_collisions = true;
										break;
									}
								}
								dll_constraints[dll_index] |= buckets[hashcode].unreferenced_functions_dll_mask;
							}
						}
						else
						{
							buckets[hashcode].unreferenced_functions_dll_mask |= dll_mask;
							if(old_referenced)
							{
								int old_dll_index = buckets[hashcode].referenced_function_dll_index - 1;
								if(old_dll_index == dll_index)
								{
									has_collisions = true;
									break;
								}
								dll_constraints[old_dll_index] |= dll_mask;
							}
						}
							
					}
					dll_index++;

					if(has_collisions)
					{
						break;
					}
				}

				if(!has_collisions && solve_constraints(dll_constraints, &new_dll_order[0]))
				{
					Concurrency::critical_section::scoped_lock l(cs);
					if(num_bits < best_num_bits || high_byte < best_high_byte)
					{
						best_low_byte = low_byte;
						best_high_byte = high_byte;
						best_num_bits = num_bits;
						best_dll_order = new_dll_order;
					}
					break;
				}
			}

			
			delete[] buckets;
		});

		int best_family = (best_high_byte << 16) | (best_low_byte << 8) | 1;
		printf("num_bits: %d: family: %8x\n", num_bits, best_family);
		if(best_num_bits > num_bits)
		{
			break;
		}
	}
	int best_family = (best_high_byte << 16) | (best_low_byte << 8) | 1;

	printf("time spent: %dms\n", GetTickCount()-stime);
	printf("done looking for hash family\n"); fflush(stdout);

	if(best_num_bits == INT_MAX)
	{
		return false;
	}

	// reorder dlls
	std::vector<std::string> new_dlls(num_dlls);
	for(int i = 0; i < num_dlls; i++)
	{
		new_dlls[i] = dlls[best_dll_order[i]];
	}
	dlls = new_dlls;
	
	hash_family = best_family;
	hash_bits = best_num_bits;
	return true;
}

HunkList* ImportHandler::createImportHunks1K(HunkList* hunklist, bool verbose, int& hash_bits, int& max_dll_name_length) {
	if(verbose)
		printf("\n-Imports----------------------------------\n");

	vector<Hunk*> importHunks;
	set<string> dll_set;

	bool found_kernel32 = false;

	//fill list for import hunks
	for(int i = 0; i < hunklist->getNumHunks(); i++)
	{
		Hunk* hunk = (*hunklist)[i];
		if(hunk->getFlags() & HUNK_IS_IMPORT)
		{
			if(strcmp(hunk->getImportDll(), "kernel32") == 0)
			{
				found_kernel32 = true;
			}
			dll_set.insert(hunk->getImportDll());
			if(getForwardRVA(hunk->getImportDll(), hunk->getImportName()) != NULL)
			{
				Log::error("", "Import '%s' from '%s' uses forwarded RVA. This feature is not supported by crinkler (yet)", hunk->getImportName(), hunk->getImportDll());
			}
			importHunks.push_back(hunk);
		}
	}

	if(!found_kernel32)
	{
		Log::error("", "Kernel32 needs to be linked for import code to function.");	//TODO: is this really how we want to handle it? if so, maybe we should move it outside the 1k compressor?
	}

	int hash_family;
	vector<string> dlls(dll_set.begin(), dll_set.end());
	if(!findCollisionFreeHashFamily(dlls, importHunks, hash_family, hash_bits))
	{
		Log::error("", "Could not find collision-free hash function");
	}

	string dllnames;
	
	int max_name_length = 0;
	for(string name : dlls)
	{
		max_name_length = max(max_name_length, (int)name.size() + 1);
	}

	for(string name : dlls)
	{
		while(dllnames.size() % max_name_length)
			dllnames.push_back(0);
		if(name.compare("kernel32") != 0)
		{
			dllnames += name;
		}
	}
	 
	Hunk* importList = new Hunk("ImportListHunk", 0, HUNK_IS_WRITEABLE, 8, 0, 65536*256);
	importList->addSymbol(new Symbol("_HashFamily", hash_family, 0, importList));
	importList->addSymbol(new Symbol("_ImportList", 0, SYMBOL_IS_RELOCATEABLE, importList));
	for(vector<Hunk*>::iterator it = importHunks.begin(); it != importHunks.end(); it++)
	{
		Hunk* importHunk = *it;
		unsigned int hashcode = hashCode_1k(importHunk->getImportName(), hash_family, hash_bits);
		importList->addSymbol(new Symbol(importHunk->getName(), hashcode*4, SYMBOL_IS_RELOCATEABLE, importList));
	}

	HunkList* newHunks = new HunkList;
	Hunk* dllNamesHunk = new Hunk("DllNames", dllnames.c_str(), HUNK_IS_WRITEABLE, 0, dllnames.size()+1, dllnames.size()+1);
	dllNamesHunk->addSymbol(new Symbol("_DLLNames", 0, SYMBOL_IS_RELOCATEABLE, dllNamesHunk));
	newHunks->addHunkBack(dllNamesHunk);
	newHunks->addHunkBack(importList);
	max_dll_name_length = max_name_length;

	return newHunks;
}