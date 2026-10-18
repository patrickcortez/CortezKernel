/**
 * @file path_resolver.hpp
 * @brief Path canonicalization ('.', '..') and directory tree traversal helper.
 */

#pragma once

#include "i_vfs_node.hpp"

namespace cortez::fs {

class PathResolver final {
public:
    static constexpr usize MAX_PATH_LEN = 256;

    static void normalize(const char* cwd,
                          const char* inputPath,
                          char* outPath,
                          usize maxOutLen) noexcept;

    static void splitParentAndLeaf(const char* canonicalPath,
                                   char* outParent,
                                   usize maxParentLen,
                                   char* outLeaf,
                                   usize maxLeafLen) noexcept;

    static Result<IVfsNode*> resolveNode(IVfsNode* rootNode,
                                         const char* canonicalPath) noexcept;
};

} // namespace cortez::fs
