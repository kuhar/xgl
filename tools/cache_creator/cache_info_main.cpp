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
#include "include/binary_cache_serialization.h"

#undef Status

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/BinaryStreamReader.h"
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
  if (auto err = inputBufferOrErr.getError()) {
    llvm::errs() << "Failed to read input file " << InFile << ": " << err.message() << "\n";
    return 3;
  }
  llvm::BinaryStreamReader inputReader((*inputBufferOrErr)->getBuffer(), llvm::support::endianness::little);
  const size_t inputBlobSize = inputReader.getLength();
  llvm::outs() << "Read: " << InFile << ", " << inputBlobSize << " B\n";

  constexpr size_t pipelineBinaryCacheHeaderSize = sizeof(vk::PipelineBinaryCachePrivateHeader);
  constexpr size_t minCacheBlobSize = vk::VkPipelineCacheHeaderDataSize + sizeof(vk::PipelineBinaryCachePrivateHeader);

  if (inputBlobSize < minCacheBlobSize) {
    llvm::errs() << "Input file too small to be a valid cache blob: " << inputBlobSize << "B < " << minCacheBlobSize
                 << "B\n";
    return 3;
  }

  const vk::PipelineCacheHeaderData *vkCacheHeader = nullptr;
  llvm::cantFail(inputReader.readObject(vkCacheHeader));



  llvm::outs() << "\n=== Vulkan Pipeline Cache Header ===\n"
               << "header length:\t\t" << vkCacheHeader->headerLength << "\n"
               << "header version:\t\t" << vkCacheHeader->headerVersion << "\n"
               << "vendor ID:\t\t" << llvm::format("0x%" PRIx32, vkCacheHeader->vendorID) << "\n"
               << "device ID:\t\t" << llvm::format("0x%" PRIx32, vkCacheHeader->deviceID) << "\n"
               << "pipeline cache UUID:\t" << cc::uuidToHexString(vkCacheHeader->UUID) << "\n";

  const int64_t trailingSpace = int64_t(vkCacheHeader->headerLength) - vk::VkPipelineCacheHeaderDataSize;
  llvm::outs() << "trailing space:\t\t" << trailingSpace << "\n";
  if (trailingSpace < 0) {
    llvm::errs() << "Header length is less then Vulkan header size. Existing cache blob analysis.\n";
    return 4;
  }

  if (vkCacheHeader->vendorID != cc::AMDVendorId) {
    llvm::errs() << "Vendor ID doesn't match the AMD vendor ID (0x1002). Exiting cache blob analysis.\n";
    return 4;
  }

  llvm::cantFail(inputReader.skip(static_cast<uint32_t>(trailingSpace)));

  const vk::PipelineBinaryCachePrivateHeader *pipelineBinaryCacheHeader = nullptr;
  llvm::cantFail(inputReader.readObject(pipelineBinaryCacheHeader));

  llvm::outs() << "\n=== Pipeline Binary Cache Private Header ===\n"
               << "header length:\t" << pipelineBinaryCacheHeaderSize << "\n"
               << "hash ID:\t"
               << llvm::format_bytes(pipelineBinaryCacheHeader->hashId, llvm::None, pipelineBinaryCacheHeaderSize)
               << "\n\n=== Cache Blob Info ===\n"
               << "content size:\t" << (inputBlobSize - minCacheBlobSize) << "\n";

  llvm::StringMap<std::string> elfMD5ToSourcePath;
  if (!ElfSourceDir.empty()) {
    llvm::SmallVector<char> rawElfSourceDir;
    if (std::error_code err = fs::real_path(ElfSourceDir, rawElfSourceDir, /* expand_tilde = */ true)) {
      llvm::errs() << "elf-source-dir " << ElfSourceDir << "could not be expanded: " << err.message() << "\n";
      return 4;
    }
    llvm::StringRef elfSourceDirReal(rawElfSourceDir.data(), rawElfSourceDir.size());

    if (!fs::is_directory(elfSourceDirReal)) {
      llvm::errs() << elfSourceDirReal << " is not a directory!\n";
      return 4;
    }
    elfMD5ToSourcePath = collectSourceElfMD5Sums(elfSourceDirReal);
  }

  size_t cacheEntryIdx = 0;
  for (; inputReader.bytesRemaining() != 0; ++cacheEntryIdx) {
    const vk::BinaryCacheEntry *entryHeader = nullptr;
    if (auto err = inputReader.readObject(entryHeader)) {
      llvm::errs() << "Failed to read binary cache entry #" << cacheEntryIdx << ". Error:\t" << err << "\n";
      llvm::consumeError(std::move(err));
      return 4;
    }

    llvm::outs() << "\n\t*** Entry " << cacheEntryIdx << " ***\n"
                 << "\thash ID:\t" << llvm::format_bytes(entryHeader->hashId.bytes) << "\n"
                 << "\tdata size:\t" << entryHeader->dataSize << "\n";

    llvm::ArrayRef<uint8_t> data;
    if (auto err = inputReader.readBytes(data, entryHeader->dataSize)) {
      llvm::errs() << "Failed to read cache entry #" << cacheEntryIdx << ". Error:\t" << err << "\n";
      llvm::consumeError(std::move(err));
      return 4;
    }

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
