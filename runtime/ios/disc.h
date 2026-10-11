#pragma once
#include <cstdint>
#include <string>

// Virtual view of the game disc.
//  * "partition" reads use the decrypted DATA partition, reconstructed from the
//    files DolphinTool extracted (sys/ + files/) laid out according to the FST.
//  * "raw" reads (unencrypted area, partition table) come straight from the ISO.
bool Disc_Open(const std::string& extractedDataDir, const std::string& isoPath);
bool Disc_ReadPartition(uint64_t offset, uint32_t length, uint8_t* out);
bool Disc_ReadRaw(uint64_t offset, uint32_t length, uint8_t* out);
const uint8_t* Disc_Header();          // 0x440 bytes of boot.bin
const std::string& Disc_DataDir();
uint64_t Disc_PartitionOffset();
bool Disc_ReadFileByPath(const std::string& discPath, std::string& out);
