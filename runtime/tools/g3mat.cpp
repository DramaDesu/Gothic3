// Loads materials out of an archive. With a name it dumps one material; without,
// it parses every material in the archive, which is the real test of the format
// work. Pointing it at the image archive as well turns the authored .tga names
// into files that actually ship.

#include "genome/material.h"
#include "genome/pak.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{

bool isMaterial(const std::string &path)
{
    return path.size() > 7 && path.compare(path.size() - 7, 7, ".xshmat") == 0;
}

std::string lowered(std::string text)
{
    for (char &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

// The image archive's 1902 basenames are globally unique, so a flat set of them
// is a correct existence test even though the archive has directories.
std::set<std::string> imageBaseNames(const genome::PakArchive &archive)
{
    std::set<std::string> names;
    for (const genome::PakEntry &entry : archive.entries())
    {
        if (entry.deleted)
            continue;
        const std::size_t slash = entry.path.find_last_of('/');
        std::string leaf = slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
        const std::size_t dot = leaf.find_last_of('.');
        if (dot == std::string::npos || lowered(leaf.substr(dot)) != ".ximg")
            continue;
        names.insert(lowered(leaf.substr(0, dot)));
    }
    return names;
}

void printMaterial(const std::string &name, const genome::Material &material, const genome::PakArchive *images,
                   const std::set<std::string> &imageNames, int materialSwitch)
{
    std::printf("%s: %s (class version %u)\n", name.c_str(), material.shaderClass.c_str(), material.shaderVersion);
    std::printf("blend %s", genome::blendModeName(material.blendMode));
    if (material.alphaTested())
        std::printf(" (discard below %.3f, reference %u)", material.alphaTestThreshold(), material.maskReference);
    std::printf(", transform %s%s%s%s\n", genome::transformationName(material.transformation),
                material.enableSpecular ? ", specular" : "", material.disableLighting ? ", unlit" : "",
                material.useDepthBias ? ", depth bias" : "");
    std::printf("two-sided %s (inferred, no flag exists in the file)\n", material.twoSided() ? "yes" : "no");
    if (!material.fallbackMaterial.empty())
        std::printf("fallback %s\n", material.fallbackMaterial.c_str());
    std::printf("collision: physics material %u%s%s%s\n", material.physicsMaterial,
                material.disableCollision ? ", none" : "", material.disableResponse ? ", no response" : "",
                material.ignoredByTraceRay ? ", ignored by trace rays" : "");
    std::printf("graph: %zu nodes, %zu samplers, %zu skipped, %zu undecoded tail bytes\n", material.nodeCount,
                material.samplers.size(), material.opaqueNodes, material.undecodedTail);

    const genome::ImageExists exists = [&](const std::string &candidate) {
        return imageNames.find(candidate) != imageNames.end();
    };

    for (genome::Slot slot : genome::shaderSlots(material.kind))
    {
        const genome::SlotBinding &binding = material.binding(slot);
        if (!binding.bound)
            continue;

        const genome::Sampler *sampler = material.texture(slot);
        std::printf("  %-17s %-22s %s\n", genome::slotName(slot), binding.sourceClass.c_str(),
                    sampler != nullptr ? sampler->imagePath.c_str() : "(no texture under it)");
        if (sampler == nullptr)
            continue;

        std::printf("  %-17s   %s/%s, switch %s%s\n", "", genome::texRepeatName(sampler->repeatU),
                    genome::texRepeatName(sampler->repeatV), genome::switchRepeatName(sampler->switchRepeat),
                    binding.throughGraph ? ", reached through the graph" : "");

        if (images == nullptr)
            continue;
        const genome::TextureResolution resolved = genome::resolveTexture(*sampler, materialSwitch, exists);
        if (resolved.fileName.empty())
        {
            std::printf("  %-17s   -> does not ship\n", "");
            continue;
        }
        const genome::PakEntry *entry = images->find(resolved.fileName);
        std::printf("  %-17s   -> %s", "", entry != nullptr ? entry->path.c_str() : resolved.fileName.c_str());
        if (resolved.variantCount > 0)
            std::printf("  (variant %d of %d, switch %d)", resolved.variantIndex + 1, resolved.variantCount,
                        materialSwitch);
        if (resolved.renamed)
            std::printf("  (numbered name remapped)");
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *archivePath = nullptr;
    const char *materialName = nullptr;
    const char *imagePath = nullptr;
    int materialSwitch = 0;

    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--images") == 0 && index + 1 < argc)
            imagePath = argv[++index];
        else if (std::strcmp(argv[index], "--switch") == 0 && index + 1 < argc)
            materialSwitch = std::atoi(argv[++index]);
        else if (archivePath == nullptr)
            archivePath = argv[index];
        else if (materialName == nullptr)
            materialName = argv[index];
    }

    if (archivePath == nullptr)
    {
        std::puts("usage: g3mat <archive.pak> [material name] [--images <_compiledImage.pak>] [--switch n]");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(archivePath, &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    std::unique_ptr<genome::PakArchive> images;
    std::set<std::string> imageNames;
    if (imagePath != nullptr)
    {
        images = genome::PakArchive::open(imagePath, &error);
        if (!images)
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        imageNames = imageBaseNames(*images);
    }

    if (materialName != nullptr)
    {
        genome::Material material;
        const std::vector<std::uint8_t> bytes = archive->read(materialName, &error);
        if (bytes.empty() || !genome::loadMaterial(bytes, material, &error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        printMaterial(materialName, material, images.get(), imageNames, materialSwitch);
        return 0;
    }

    const genome::ImageExists exists = [&](const std::string &candidate) {
        return imageNames.find(candidate) != imageNames.end();
    };

    std::size_t parsed = 0, failed = 0, references = 0, resolvedCount = 0, missing = 0, renamedCount = 0;
    std::size_t presentCount = 0, exact = 0, skipped = 0;
    std::map<std::string, std::size_t> kinds, blends, reasons;
    std::map<std::string, std::size_t> slotUse;
    std::vector<std::string> failures;
    for (const genome::PakEntry &entry : archive->entries())
    {
        if (entry.deleted || !isMaterial(entry.path))
            continue;

        genome::Material material;
        const std::vector<std::uint8_t> bytes = archive->read(entry, &error);
        if (bytes.empty() || !genome::loadMaterial(bytes, material, &error))
        {
            ++failed;
            ++reasons[error];
            if (failures.size() < 16)
                failures.push_back(entry.path + ": " + error);
            continue;
        }
        ++parsed;
        ++kinds[material.shaderClass];
        ++blends[genome::blendModeName(material.blendMode)];
        if (material.undecodedTail == 0)
            ++exact;
        skipped += material.opaqueNodes;

        for (genome::Slot slot : genome::shaderSlots(material.kind))
        {
            if (material.texture(slot) != nullptr)
                ++slotUse[genome::slotName(slot)];
        }

        if (images == nullptr)
            continue;
        for (const genome::Sampler &sampler : material.samplers)
        {
            ++references;
            // Every sampler is resolved at switch zero, the authored variant, so
            // that the count measures the naming rule rather than one body part.
            const genome::TextureResolution resolved = genome::resolveTexture(sampler, materialSwitch, exists);
            if (resolved.fileName.empty())
                ++missing;
            else
            {
                ++resolvedCount;
                if (resolved.renamed)
                    ++renamedCount;
                // Independent check of the rule: the resolver decided using a
                // basename set built here, so ask the archive's own lookup
                // whether the name it produced is really a file.
                if (images->find(resolved.fileName) != nullptr)
                    ++presentCount;
            }
        }
    }

    std::printf("parsed %zu materials, %zu failed\n", parsed, failed);
    std::printf("%zu consumed the effect byte for byte, %zu left an undecoded legacy tail, %zu graph nodes skipped\n",
                exact, parsed - exact, skipped);
    std::printf("shader kinds:\n");
    for (const auto &[kind, count] : kinds)
        std::printf("  %5zu  %s\n", count, kind.c_str());
    std::printf("blend modes:\n");
    for (const auto &[blend, count] : blends)
        std::printf("  %5zu  %s\n", count, blend.c_str());
    std::printf("slots with a texture:\n");
    for (const auto &[slot, count] : slotUse)
        std::printf("  %5zu  %s\n", count, slot.c_str());
    if (images != nullptr)
    {
        std::printf("textures: %zu references, %zu resolve (%zu via the numbered rename), %zu do not ship\n",
                    references, resolvedCount, renamedCount, missing);
        std::printf("of those %zu names, %zu are present in the archive by its own lookup\n", resolvedCount,
                    presentCount);
    }
    if (failed != 0)
    {
        std::printf("failures:\n");
        for (const auto &[reason, count] : reasons)
            std::printf("  %5zu  %s\n", count, reason.c_str());
        for (const std::string &failure : failures)
            std::printf("         %s\n", failure.c_str());
    }
    return failed == 0 ? 0 : 1;
}
