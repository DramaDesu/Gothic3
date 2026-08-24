#pragma once

// Minimal JSON writer. The MCP bridge is the only consumer, so this covers
// exactly the value kinds the handlers emit and nothing more.

#include <g3sdk/SharedBase.h>

class mCJsonWriter
{
  public:
    mCJsonWriter() : m_bFirst(GETrue)
    {
        m_Buffer = "{";
    }

    mCJsonWriter &Str(GELPCChar a_pKey, bCString const &a_Value)
    {
        Separator(a_pKey);
        m_Buffer += "\"";
        m_Buffer += Escape(a_Value);
        m_Buffer += "\"";
        return *this;
    }

    mCJsonWriter &Int(GELPCChar a_pKey, GEI32 a_Value)
    {
        Separator(a_pKey);
        bCString Tmp;
        Tmp.Format("%d", a_Value);
        m_Buffer += Tmp;
        return *this;
    }

    mCJsonWriter &Float(GELPCChar a_pKey, GEFloat a_Value)
    {
        Separator(a_pKey);
        bCString Tmp;
        Tmp.Format("%.3f", a_Value);
        m_Buffer += Tmp;
        return *this;
    }

    mCJsonWriter &Bool(GELPCChar a_pKey, GEBool a_Value)
    {
        Separator(a_pKey);
        m_Buffer += a_Value ? "true" : "false";
        return *this;
    }

    // Raw JSON (already-formed object/array), used for nesting.
    mCJsonWriter &Raw(GELPCChar a_pKey, bCString const &a_Json)
    {
        Separator(a_pKey);
        m_Buffer += a_Json;
        return *this;
    }

    mCJsonWriter &Vector(GELPCChar a_pKey, bCVector const &a_Vector)
    {
        bCString Tmp;
        Tmp.Format("[%.3f,%.3f,%.3f]", a_Vector.GetX(), a_Vector.GetY(), a_Vector.GetZ());
        return Raw(a_pKey, Tmp);
    }

    bCString Finish()
    {
        m_Buffer += "}";
        return m_Buffer;
    }

    static bCString Escape(bCString const &a_Value)
    {
        bCString Out;
        GELPCChar p = a_Value.GetText();
        for (GEInt i = 0; p && p[i]; i++)
        {
            char c = p[i];
            switch (c)
            {
                case '"': Out += "\\\""; break;
                case '\\': Out += "\\\\"; break;
                case '\n': Out += "\\n"; break;
                case '\r': Out += "\\r"; break;
                case '\t': Out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        bCString Tmp;
                        Tmp.Format("\\u%04x", static_cast<unsigned char>(c));
                        Out += Tmp;
                    }
                    else
                    {
                        char sz[2] = {c, 0};
                        Out += sz;
                    }
                    break;
            }
        }
        return Out;
    }

  private:
    void Separator(GELPCChar a_pKey)
    {
        if (!m_bFirst)
            m_Buffer += ",";
        m_bFirst = GEFalse;
        m_Buffer += "\"";
        m_Buffer += a_pKey;
        m_Buffer += "\":";
    }

    bCString m_Buffer;
    GEBool m_bFirst;
};

// Flat request parser: the bridge only ever sends {"cmd":"...","key":"value",...}
// with string/number values and no nesting, so a scanner beats a real parser here.
class mCJsonRequest
{
  public:
    explicit mCJsonRequest(bCString const &a_Json) : m_Json(a_Json)
    {}

    bCString GetString(GELPCChar a_pKey, GELPCChar a_pDefault = "") const
    {
        GEInt iValue = FindValue(a_pKey);
        if (iValue < 0)
            return bCString(a_pDefault);

        GELPCChar p = m_Json.GetText();
        if (p[iValue] != '"')
            return bCString(a_pDefault);

        bCString Out;
        for (GEInt i = iValue + 1; p[i]; i++)
        {
            if (p[i] == '\\' && p[i + 1])
            {
                char c = p[++i];
                char sz[2] = {c == 'n' ? '\n' : c == 't' ? '\t' : c, 0};
                Out += sz;
                continue;
            }
            if (p[i] == '"')
                break;
            char sz[2] = {p[i], 0};
            Out += sz;
        }
        return Out;
    }

    GEFloat GetFloat(GELPCChar a_pKey, GEFloat a_fDefault = 0.0f) const
    {
        GEInt iValue = FindValue(a_pKey);
        if (iValue < 0)
            return a_fDefault;
        return static_cast<GEFloat>(atof(m_Json.GetText() + iValue));
    }

    GEBool Has(GELPCChar a_pKey) const
    {
        return FindValue(a_pKey) >= 0;
    }

  private:
    GEInt FindValue(GELPCChar a_pKey) const
    {
        bCString Needle;
        Needle.Format("\"%s\"", a_pKey);
        GEInt iKey = m_Json.Find(Needle);
        if (iKey < 0)
            return -1;
        GELPCChar p = m_Json.GetText();
        GEInt i = iKey + Needle.GetLength();
        while (p[i] == ' ' || p[i] == ':')
            i++;
        return p[i] ? i : -1;
    }

    bCString m_Json;
};
