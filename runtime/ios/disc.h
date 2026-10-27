#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ModInfo;

// Virtual view of the game disc.
//  * "partition" reads use the decrypted DATA partition, reconstructed from the
//    installed files (sys/ + files/, see install/install.h) laid out according to the FST.
//  * "raw" reads (unencrypted area, partition table) come from disc/unencrypted.bin, or
//    from the ISO for data extracted with DolphinTool.
//  * mods (mods/mods.h) replace files of the partition: their files are read instead, and
//    the FST the game sees has their sizes. The installed files are never changed.
bool Disc_Open(const std::string& extractedDataDir, const std::string& isoPath, const std::vector<ModInfo>& mods);
bool Disc_ReadPartition(uint64_t offset, uint32_t length, uint8_t* out);
bool Disc_ReadRaw(uint64_t offset, uint32_t length, uint8_t* out);
const uint8_t* Disc_Header();          // 0x440 bytes of boot.bin
const std::string& Disc_DataDir();
uint64_t Disc_PartitionOffset();
// fst.bin as the game sees it (with the mods' offsets and sizes)
const std::vector<uint8_t>& Disc_Fst();
bool Disc_ReadFileByPath(const std::string& discPath, std::string& out);
