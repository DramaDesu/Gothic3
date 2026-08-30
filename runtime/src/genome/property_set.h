#pragma once

// The envelope every Genome resource shares: an optional GENOMFLE wrapper whose
// string table turns strings into two-byte indices, then a property-set header
// naming the class and listing its declared properties.

#include "reader.h"

#include <optional>
#include <string>
#include <vector>

namespace genome
{

struct Property
{
    std::string name;
    std::string type;
    std::vector<std::uint8_t> value;

    // Convenience accessors for the handful of types we actually read.
    std::optional<float> asFloat() const;
    std::optional<std::string> asString() const;
    bool asBox(float outMin[3], float outMax[3]) const;
};

// Wrapped files address strings through a table at the end of the file; raw
// files store them inline. Both extensions coexist in the same archive, so the
// wrapper has to be sniffed rather than assumed.
class StringTable
{
  public:
    // Reads the table if the file is wrapped, leaving the cursor at the payload.
    static StringTable sniff(Reader &reader);

    bool wrapped() const { return m_wrapped; }
    std::size_t payloadEnd() const { return m_payloadEnd; }

    // In a wrapped file this consumes a two-byte index; otherwise a full string.
    std::string entry(Reader &reader) const;

  private:
    bool m_wrapped = false;
    std::size_t m_payloadEnd = 0;
    std::vector<std::string> m_strings;
};

struct PropertySetHeader
{
    std::string className;
    std::string objectName;   // only present below version 81
    std::uint16_t version = 0;
    std::size_t declaredEnd = 0;  // where the class body must end
    std::vector<Property> properties;
    std::uint16_t classVersion = 0;

    const Property *find(std::string_view name) const;
};

// Reads the common header. On success the cursor sits at the class body and
// `declaredEnd` says where that body finishes - always seek there afterwards
// rather than trusting the body to consume itself exactly.
bool readPropertySetHeader(Reader &reader, const StringTable &strings, PropertySetHeader &header);

// The prefix every resource class body starts with (eCResourceBase_PS).
bool readResourceBase(Reader &reader, std::uint16_t &resourceVersion);

} // namespace genome
