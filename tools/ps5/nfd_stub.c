// nativefiledialog-extended's API (nfd.h) for the PS5, where a title has no file dialogs: RT64 links
// nfd, and on the console this stands in for it. Initialisation succeeds, every dialog reports that it
// was cancelled and every path set is empty. The game finds the ROM in the title folder instead
// (src/platform/ps5_platform.cpp).
#include <stddef.h>

#include "nfd.h"

nfdresult_t NFD_Init(void) {
    return NFD_OKAY;
}

void NFD_Quit(void) {}

// No dialog hands out a path, so there is never one to free.
void NFD_FreePathN(nfdnchar_t* filePath) {
    (void)filePath;
}

void NFD_FreePathU8(nfdu8char_t* filePath) {
    (void)filePath;
}

nfdresult_t NFD_OpenDialogN(nfdnchar_t** outPath, const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount, const nfdnchar_t* defaultPath) {
    (void)filterList;
    (void)filterCount;
    (void)defaultPath;
    if (outPath != NULL) {
        *outPath = NULL;
    }
    return NFD_CANCEL;
}

nfdresult_t NFD_OpenDialogU8(nfdu8char_t** outPath, const nfdu8filteritem_t* filterList,
                             nfdfiltersize_t filterCount, const nfdu8char_t* defaultPath) {
    return NFD_OpenDialogN(outPath, filterList, filterCount, defaultPath);
}

nfdresult_t NFD_OpenDialogMultipleN(const nfdpathset_t** outPaths, const nfdnfilteritem_t* filterList,
                                    nfdfiltersize_t filterCount, const nfdnchar_t* defaultPath) {
    (void)filterList;
    (void)filterCount;
    (void)defaultPath;
    if (outPaths != NULL) {
        *outPaths = NULL;
    }
    return NFD_CANCEL;
}

nfdresult_t NFD_OpenDialogMultipleU8(const nfdpathset_t** outPaths, const nfdu8filteritem_t* filterList,
                                     nfdfiltersize_t filterCount, const nfdu8char_t* defaultPath) {
    return NFD_OpenDialogMultipleN(outPaths, filterList, filterCount, defaultPath);
}

nfdresult_t NFD_SaveDialogN(nfdnchar_t** outPath, const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount, const nfdnchar_t* defaultPath,
                            const nfdnchar_t* defaultName) {
    (void)defaultName;
    return NFD_OpenDialogN(outPath, filterList, filterCount, defaultPath);
}

nfdresult_t NFD_SaveDialogU8(nfdu8char_t** outPath, const nfdu8filteritem_t* filterList,
                             nfdfiltersize_t filterCount, const nfdu8char_t* defaultPath,
                             const nfdu8char_t* defaultName) {
    return NFD_SaveDialogN(outPath, filterList, filterCount, defaultPath, defaultName);
}

nfdresult_t NFD_PickFolderN(nfdnchar_t** outPath, const nfdnchar_t* defaultPath) {
    return NFD_OpenDialogN(outPath, NULL, 0, defaultPath);
}

nfdresult_t NFD_PickFolderU8(nfdu8char_t** outPath, const nfdu8char_t* defaultPath) {
    return NFD_PickFolderN(outPath, defaultPath);
}

// Nothing fails, so there is no error to report.
const char* NFD_GetError(void) {
    return NULL;
}

void NFD_ClearError(void) {}

nfdresult_t NFD_PathSet_GetCount(const nfdpathset_t* pathSet, nfdpathsetsize_t* count) {
    (void)pathSet;
    if (count != NULL) {
        *count = 0;
    }
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_GetPathN(const nfdpathset_t* pathSet, nfdpathsetsize_t index, nfdnchar_t** outPath) {
    (void)pathSet;
    (void)index;
    if (outPath != NULL) {
        *outPath = NULL;
    }
    return NFD_ERROR;  // an empty set has no path at any index
}

nfdresult_t NFD_PathSet_GetPathU8(const nfdpathset_t* pathSet, nfdpathsetsize_t index, nfdu8char_t** outPath) {
    return NFD_PathSet_GetPathN(pathSet, index, outPath);
}

void NFD_PathSet_FreePathN(const nfdnchar_t* filePath) {
    (void)filePath;
}

void NFD_PathSet_FreePathU8(const nfdu8char_t* filePath) {
    (void)filePath;
}

nfdresult_t NFD_PathSet_GetEnum(const nfdpathset_t* pathSet, nfdpathsetenum_t* outEnumerator) {
    (void)pathSet;
    if (outEnumerator != NULL) {
        outEnumerator->ptr = NULL;
    }
    return NFD_OKAY;
}

void NFD_PathSet_FreeEnum(nfdpathsetenum_t* enumerator) {
    (void)enumerator;
}

nfdresult_t NFD_PathSet_EnumNextN(nfdpathsetenum_t* enumerator, nfdnchar_t** outPath) {
    (void)enumerator;
    if (outPath != NULL) {
        *outPath = NULL;  // the end of the set
    }
    return NFD_OKAY;
}

nfdresult_t NFD_PathSet_EnumNextU8(nfdpathsetenum_t* enumerator, nfdu8char_t** outPath) {
    return NFD_PathSet_EnumNextN(enumerator, outPath);
}

void NFD_PathSet_Free(const nfdpathset_t* pathSet) {
    (void)pathSet;
}
