/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Google LLC. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "cache_creator.h"
#include "palInlineFuncs.h"
#include "include/binary_cache_serialization.h"

#if defined(Status)
#undef Status
#endif

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MD5.h"
#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <iostream>
#include <string>

namespace {
llvm::cl::OptionCategory CacheInfoCat("Cache Info Options");

llvm::cl::opt<std::string> InFile(llvm::cl::Positional, llvm::cl::ValueRequired, llvm::cl::cat(CacheInfoCat),
                                  llvm::cl::desc("<Input cache_file.bin>"));
llvm::cl::opt<std::string> ElfSourceDir("elf-source-dir", llvm::cl::desc("Directory with source elf files"),
                                        llvm::cl::cat(CacheInfoCat), llvm::cl::value_desc("directory"));
} // namespace

namespace fs = llvm::sys::fs;

llvm::StringMap<std::string> collectSourceElfMD5Sums(llvm::Twine dir) {
  llvm::StringMap<std::string> md5ToElfPath;

  std::error_code ec{};
  for (fs::recursive_directory_iterator it{dir, ec}, e{}; it != e && !ec; it.increment(ec)) {
    const fs::directory_entry &entry = *it;
    const std::string &path(entry.path());
    if (!llvm::StringRef(path).endswith(".elf") || fs::is_directory(path))
      continue;

    llvm::ErrorOr<llvm::MD5::MD5Result> elfMD5OrErr = fs::md5_contents(path);
    if (std::error_code err = elfMD5OrErr.getError()) {
      llvm::errs() << "[WARN] Can not read source elf file " << path << ": " << err.message() << "\n";
      continue;
    }

    md5ToElfPath.insert({elfMD5OrErr->digest(), path});
  }

  return md5ToElfPath;
}

int main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(argc, argv);

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> inputBufferOrErr = llvm::MemoryBuffer::getFile(InFile);
  if (std::error_code err = inputBufferOrErr.getError()) {
    llvm::errs() << "Failed to read input file " << InFile << ": " << err.message() << "\n";
    return 3;
  }
  std::unique_ptr<llvm::MemoryBuffer> inputBuffer(std::move(*inputBufferOrErr));
  llvm::outs() << "Read: " << InFile << ", " << inputBuffer->getBufferSize() << " B\n";

  constexpr size_t pipelineBinaryCacheHeaderSize = sizeof(vk::PipelineBinaryCachePrivateHeader);
  constexpr size_t minCacheBlobSize = vk::VkPipelineCacheHeaderDataSize + pipelineBinaryCacheHeaderSize;

  const size_t inputBlobSize = inputBuffer->getBufferSize();
  if (inputBlobSize < minCacheBlobSize) {
    llvm::errs() << "Input file too small to be a valid cache blob: " << inputBlobSize << "B < " << minCacheBlobSize
                 << "B\n";
    return 3;
  }

  auto *inputBufferBegin = reinterpret_cast<const uint8_t *>(inputBuffer->getBufferStart());
  const uint8_t *currDataPos = inputBufferBegin;
  auto *vkCacheHeader = reinterpret_cast<const vk::PipelineCacheHeaderData *>(currDataPos);
  currDataPos += vk::VkPipelineCacheHeaderDataSize;

  llvm::outs() << "\n=== Vulkan Pipeline Cache Header ===\n"
               << "header length:\t\t" << vkCacheHeader->headerLength << "\n"
               << "header version:\t\t" << vkCacheHeader->headerVersion << "\n"
               << "vendor ID:\t\t" << llvm::format("0x%" PRIx32, vkCacheHeader->vendorID) << "\n"
               << "device ID:\t\t" << llvm::format("0x%" PRIx32, vkCacheHeader->deviceID) << "\n"
               << "pipeline cache UUID:\t" << cc::uuidToHexString(vkCacheHeader->UUID) << "\n";

  if (vkCacheHeader->vendorID != cc::AMDVendorId) {
    llvm::errs() << "Vendor ID doesn't match the AMD vendor ID (0x1002). Exiting cache blob analysis.\n";
    return 4;
  }

  auto *pipelineBinaryCacheHeader = reinterpret_cast<const vk::PipelineBinaryCachePrivateHeader *>(currDataPos);
  currDataPos += pipelineBinaryCacheHeaderSize;

  llvm::outs() << "\n=== Pipeline Binary Cache Private Header ===\n"
               << "header length:\t" << pipelineBinaryCacheHeaderSize << "\n"
               << "hash ID:\t"
               << llvm::format_bytes(pipelineBinaryCacheHeader->hashId, llvm::None, pipelineBinaryCacheHeaderSize)
               << "\n";

  llvm::outs() << "\n=== Cache Blob Info ===\n"
               << "content size:\t" << (inputBlobSize - minCacheBlobSize) << "\n";

  llvm::StringMap<std::string> elfMD5ToSourcePath;
  if (!ElfSourceDir.empty()) {
    llvm::SmallVector<char> rawElfSourceDir;
    if (std::error_code err = fs::real_path(ElfSourceDir, rawElfSourceDir, /* expand_tilde = */ true)) {
      llvm::errs() << "elf-source-dir " << ElfSourceDir << "could not be expanded: " << err.message() << "\n";
      return 4;
    }
    llvm::StringRef elfSourceDirReal(rawElfSourceDir.begin(), rawElfSourceDir.size());

    if (!fs::is_directory(elfSourceDirReal)) {
      llvm::errs() << elfSourceDirReal << " is not a directory!\n";
      return 4;
    }
    elfMD5ToSourcePath = collectSourceElfMD5Sums(elfSourceDirReal);
  }

  auto *inputBufferEnd = reinterpret_cast<const uint8_t *>(inputBuffer->getBufferEnd());
  size_t cacheEntryIdx = 0;
  for (; currDataPos < inputBufferEnd; ++cacheEntryIdx) {
    auto *entryHeader = reinterpret_cast<const vk::BinaryCacheEntry *>(currDataPos);
    currDataPos += sizeof(vk::BinaryCacheEntry);

    llvm::outs() << "\n\t*** Entry " << cacheEntryIdx << " ***\n"
                 << "\thash ID:\t" << llvm::format_bytes(entryHeader->hashId.bytes) << "\n"
                 << "\tdata size:\t" << entryHeader->dataSize << "\n";

    const size_t remainingBlobBytes = inputBufferEnd - currDataPos;
    if (entryHeader->dataSize > remainingBlobBytes) {
      llvm::errs() << "[WARN] Entry data size exceeds cache blob content size!\n";
      return 4;
    }

    llvm::ArrayRef<uint8_t> data(reinterpret_cast<const uint8_t *>(currDataPos), entryHeader->dataSize);
    if (data.end() > inputBufferEnd)
      break;

    currDataPos += entryHeader->dataSize;

    llvm::MD5 md5;
    md5.update(data);
    llvm::MD5::MD5Result result{};
    md5.final(result);
    auto digest = result.digest();
    llvm::outs() << "\tMD5 sum:\t" << digest << "\n";

    if (!elfMD5ToSourcePath.empty()) {
      auto it = elfMD5ToSourcePath.find(digest);
      if (it != elfMD5ToSourcePath.end()) {
        llvm::outs() << "\tsource elf:\t" << it->second << "\n";
      } else {
        llvm::outs() << "\tno matching source found\n";
      }
    }
  }

  llvm::outs() << "\nTotal num entries:\t" << cacheEntryIdx << "\n";

  return 0;
}
