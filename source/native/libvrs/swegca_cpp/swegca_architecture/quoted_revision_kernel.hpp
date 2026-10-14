#pragma once
#include <optional>
#include <string_view>

namespace swegca::architecture::kernel {
struct TextSpan {std::size_t offset=0,bytes=0;};
struct QuotedRevision {TextSpan prior,replacement;};
// Parse only an explicit whole-line correction proposal. These spans are
// syntax, never a verified semantic delta, evidence outcome or mutation permit.
// Unrecognized/ambiguous syntax remains unresolved. No allocation or model.
[[nodiscard]] inline std::optional<QuotedRevision> quoted_revision(std::string_view text) noexcept {
    std::size_t at=0;
    const auto spaces=[&]{while(at<text.size()&&(text[at]==' '||text[at]=='\t'))++at;};
    const auto take=[&](std::string_view token){if(!text.substr(at).starts_with(token))return false;at+=token.size();return true;};
    spaces();
    if(!take("정정:")&&!take("Correction:")&&!take("correction:"))return std::nullopt;
    spaces();
    const auto quoted=[&]() -> std::optional<TextSpan> {
        std::string_view close;
        if(take("\""))close="\"";else if(take("“"))close="”";else return std::nullopt;
        const auto start=at,end=text.find(close,at);
        if(end==text.npos||end==start)return std::nullopt;
        const auto value=text.substr(start,end-start);
        if(value.find_first_of("\\\r\n\"")!=value.npos||value.find("“")!=value.npos||value.find("”")!=value.npos)return std::nullopt;
        at=end+close.size();return TextSpan{start,end-start};
    };
    const auto prior=quoted();if(!prior)return std::nullopt;
    spaces();if(!take("->")&&!take("→"))return std::nullopt;spaces();
    const auto replacement=quoted();if(!replacement)return std::nullopt;
    spaces();(void)take(".");spaces();(void)take("\r");(void)take("\n");
    if(at!=text.size())return std::nullopt;
    return QuotedRevision{*prior,*replacement};
}
} // namespace swegca::architecture::kernel
