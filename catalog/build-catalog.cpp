#include "pch.h"
#include <iostream>
#include <filesystem>
#include "Microsoft/SQLiteIndex.h"

using namespace AppInstaller::Repository::Microsoft;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <catalog.db> <manifest_dir>" << std::endl;
        return 1;
    }

    std::string dbPath = argv[1];
    std::string manifestDir = argv[2];

    try {
        auto index = SQLiteIndex::CreateNew(dbPath);
        
        int count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(manifestDir)) {
            if (entry.path().extension() == ".yaml" || entry.path().extension() == ".yml") {
                std::string fileName = entry.path().filename().string();
                std::string fullPath = entry.path().string();
                
                std::cout << "Indexing " << fileName << "..." << std::endl;
                index.AddManifest(fullPath, fileName);
                count++;
            }
        }
        
        std::cout << "Successfully indexed " << count << " manifests into " << dbPath << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}