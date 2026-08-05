#include "session/zenz_diagnostic.h"

#include "testing/gunit.h"

namespace mozc {
namespace session {
namespace {

TEST(ZenzDiagnosticJsonTest, PreservesBinaryFieldsAsBase64) {
  ZenzDiagnosticJson json;
  json.AddString("event", "request");
  json.AddBase64("prompt_base64", std::string("a\0b", 3));
  json.AddCodepoints("prompt_codepoints", "A\xEE\xB8\x82");

  EXPECT_EQ(json.Build(),
            "{\"event\":\"request\",\"prompt_base64\":\"YQBi\","
            "\"prompt_codepoints\":[\"U+0041\",\"U+EE02\"]}");
}

TEST(ZenzDiagnosticJsonTest, EscapesStrings) {
  ZenzDiagnosticJson json;
  json.AddString("value", "quote\" slash\\ line\n");
  EXPECT_EQ(json.Build(),
            "{\"value\":\"quote\\\" slash\\\\ line\\n\"}");
}

}  // namespace
}  // namespace session
}  // namespace mozc
