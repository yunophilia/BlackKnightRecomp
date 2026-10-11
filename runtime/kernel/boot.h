#pragma once
#include <cstdint>
#include <string>

// Prepare guest memory as the system menu + apploader would; returns the DOL entry point.
uint32_t Boot_Setup(const std::string& extractedDataDir);
