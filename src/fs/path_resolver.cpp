/**
 * @file path_resolver.cpp
 * @brief Implementation of path canonicalization and VFS node traversal.
 */

#include "path_resolver.hpp"
#include "../lib/string_utils.hpp"

namespace cortez::fs {

void PathResolver::normalize(const char* cwd,
                             const char* inputPath,
                             char* outPath,
                             usize maxOutLen) noexcept {
    if (!outPath || maxOutLen < 2) return;

    char combined[MAX_PATH_LEN * 2]{};
    if (!inputPath || inputPath[0] == '\0') {
        str::copy(combined, (cwd && cwd[0]) ? cwd : "/", sizeof(combined));
    } else if (inputPath[0] == '/') {
        str::copy(combined, inputPath, sizeof(combined));
    } else {
        str::copy(combined, (cwd && cwd[0]) ? cwd : "/", sizeof(combined));
        usize clen = str::length(combined);
        if (clen > 0 && combined[clen - 1] != '/') {
            str::concat(combined, "/", sizeof(combined));
        }
        str::concat(combined, inputPath, sizeof(combined));
    }

    char segments[16][64]{};
    usize segCount = 0;

    const char* p = combined;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;

        char seg[64]{};
        usize len = 0;
        while (*p && *p != '/' && len + 1 < sizeof(seg)) {
            seg[len++] = *p++;
        }
        seg[len] = '\0';

        if (str::compare(seg, ".") == 0) {
            continue;
        }
        if (str::compare(seg, "..") == 0) {
            if (segCount > 0) --segCount;
            continue;
        }
        if (segCount < 16) {
            str::copy(segments[segCount++], seg, 64);
        }
    }

    if (segCount == 0) {
        str::copy(outPath, "/", maxOutLen);
        return;
    }

    outPath[0] = '\0';
    for (usize i = 0; i < segCount; ++i) {
        str::concat(outPath, "/", maxOutLen);
        str::concat(outPath, segments[i], maxOutLen);
    }
}

void PathResolver::splitParentAndLeaf(const char* canonicalPath,
                                      char* outParent,
                                      usize maxParentLen,
                                      char* outLeaf,
                                      usize maxLeafLen) noexcept {
    if (!canonicalPath || !outParent || !outLeaf) return;

    const char* lastSlash = str::findLastChar(canonicalPath, '/');
    if (!lastSlash || lastSlash == canonicalPath) {
        str::copy(outParent, "/", maxParentLen);
        const char* leafStart = lastSlash ? (lastSlash + 1) : canonicalPath;
        str::copy(outLeaf, leafStart, maxLeafLen);
        return;
    }

    usize parentLen = static_cast<usize>(lastSlash - canonicalPath);
    if (parentLen + 1 > maxParentLen) parentLen = maxParentLen - 1;
    for (usize i = 0; i < parentLen; ++i) {
        outParent[i] = canonicalPath[i];
    }
    outParent[parentLen] = '\0';

    str::copy(outLeaf, lastSlash + 1, maxLeafLen);
}

Result<IVfsNode*> PathResolver::resolveNode(IVfsNode* rootNode,
                                            const char* canonicalPath) noexcept {
    if (!rootNode || !canonicalPath) return Status::InvalidArgument;
    if (str::compare(canonicalPath, "/") == 0 || canonicalPath[0] == '\0') {
        return rootNode;
    }

    IVfsNode* current = rootNode;
    const char* p     = canonicalPath;

    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;

        if (current->getType() != NodeType::Directory) {
            return Status::NotDirectory;
        }

        char comp[64]{};
        usize len = 0;
        while (*p && *p != '/' && len + 1 < sizeof(comp)) {
            comp[len++] = *p++;
        }
        comp[len] = '\0';

        auto nextRes = current->lookup(comp);
        if (nextRes.isError()) {
            return nextRes.status();
        }
        current = nextRes.value();
    }

    return current;
}

} // namespace cortez::fs
