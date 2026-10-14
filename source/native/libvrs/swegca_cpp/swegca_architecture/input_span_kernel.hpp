#pragma once
#include <string_view>

namespace swegca::architecture::kernel {
// Position overlap only, after source/quote authentication. Subtraction avoids
// endpoint overflow; an overlapping observation is not semantic entailment.
[[nodiscard]] constexpr bool input_spans_overlap(std::size_t item_a,std::size_t begin_a,std::size_t size_a,
    std::size_t item_b,std::size_t begin_b,std::size_t size_b) noexcept {
    if(item_a!=item_b||!size_a||!size_b)return false;
    return begin_a<=begin_b?begin_b-begin_a<size_a:begin_a-begin_b<size_b;
}

// A possible structural boundary, not a sentence/requirement judgment.
// Returned slices partition the unchanged bytes. Quotes and code are kept
// intact, including an unmatched opening delimiter through the input end.
[[nodiscard]] inline std::size_t input_span_end(std::string_view text,std::size_t start) noexcept {
    if(start>=text.size())return text.size();
    std::string_view closing;std::size_t ticks=0;
    const auto horizontal=[](char c){return c==' '||c=='\t'||c=='\r';};
    for(std::size_t at=start;at<text.size();){
        if(ticks){
            if(text[at]!='`'){++at;continue;}
            auto end=at;while(end<text.size()&&text[end]=='`')++end;
            if(end-at==ticks)ticks=0;
            at=end;continue;
        }
        if(!closing.empty()){
            if(text[at]=='\\'&&at+1<text.size()){at+=2;continue;}
            if(text.substr(at).starts_with(closing)){at+=closing.size();closing={};}
            else ++at;
            continue;
        }
        if(text[at]=='`'){
            auto end=at;while(end<text.size()&&text[end]=='`')++end;
            ticks=end-at;at=end;continue;
        }
        if(text[at]=='"'){closing="\"";++at;continue;}
        if(text.substr(at).starts_with("“")){closing="”";at+=3;continue;}
        if(text.substr(at).starts_with("‘")){closing="’";at+=3;continue;}
        // ASCII apostrophes inside a word do not open a quoted passage.
        const auto word=[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c>=128;};
        if(text[at]=='\''&&(at==start||!word(static_cast<unsigned char>(text[at-1])))){
            closing="'";++at;continue;
        }
        if(text[at]=='\n')return at+1;
        if(text[at]=='.'||text[at]=='?'||text[at]=='!'){
            // Preserve numbered markers, decimals and non-spaced URLs/tokens.
            if(text[at]=='.'&&at>start&&text[at-1]>='0'&&text[at-1]<='9'){++at;continue;}
            auto end=at+1;
            while(end<text.size()&&(text[end]=='.'||text[end]=='?'||text[end]=='!'))++end;
            const auto punctuation_end=end;
            while(end<text.size()&&horizontal(text[end]))++end;
            if(end<text.size()&&text[end]=='\n')return end+1;
            if(end==text.size()||end>punctuation_end)return end;
        }
        ++at;
    }
    return text.size();
}
} // namespace swegca::architecture::kernel
