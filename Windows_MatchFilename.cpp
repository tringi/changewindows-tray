#include "Windows_MatchFilename.hpp"

/* Windows MatchFilename 
// Windows_MatchFilename.cpp
//
// Author: Jan Ringos, http://Tringi.MX-3.cz, jan@ringos.cz
// Version: 1.0
//
// Changelog:
//      16.04.2013 - initial version
*/

#include <cstddef>

namespace {
    std::size_t strlength (const wchar_t * b) {
        const wchar_t * e = b;
        while (*e) {
            ++e;
        }
        return e - b;
    }
    
    const wchar_t * next (const wchar_t * p, wchar_t m) {
        while (*p && *p != m) ++p;
        return p;
    }
    
    bool loop (const wchar_t * filename, const wchar_t * mask) {
        while (*mask) {
            switch (*mask) {
                
                // star matches any number of characters
                //  - but there are special cases
                
                case L'*':
                    switch (mask [+1]) {
                        
                        // shortcut, early success
                        //  - if star is last mask character and we didn't fail
                        //    by now, everything will match
                        
                        case L'\0':
                            return true;
                        
                        // mask ending with "*."
                        //  - DOS 8.3 backward compatibility extension
                        //  - if there is no further dot in the filename (that
                        //    is no 'extension') everything will match again
                        
                        case L'.':
                            if (mask [+2] == L'\0') {
                                if (!*next (filename, L'.'))
                                    return true;
                            }

                            [[ fallthrough ]];
                        
                        // any other character
                        //  - find all occurances of that character and
                        //    recursively try matching from that point
                        //  - note that '?' cannot follow '*' (is removed in
                        //    mask normalization)
                        
                        default:
                            const wchar_t * p = filename;
                            while (*(p = next (p, mask [+1]))) {
                                
                                // try to match rest of the string with rest
                                // of the mask; we can skip the character found
                                
                                if (loop (p + 1, mask + 2))
                                    return true;
                                
                                ++p;
                            }
                            
                            // no reason to continue matching
                            //  - next mask character is nowhere in the string
                            
                            return false;
                    }
                    break;
                
                // question mark matches any one character
                //  - pointers advance after this switch, so do nothing
                //  - except test for end of the filename string
                
                case L'?':
                    if (!*filename)
                        return false;
                    
                    break;
                
                // dot matches, except dots, also end of the filename string
                //  - also when followed by star
                //  - NOTE: this is DOS 8.3 backward compatibility extension
                
                case L'.':
                    if (mask [+1] == L'\0' || (mask [+1] == L'*' && mask [+2] == L'\0')) {
                        if (*filename == L'\0') {
                            return true;
                        }
                    }
                    [[ fallthrough ]];
                
                // other characters matches themselves
                
                default:
                    if (*filename != *mask)
                        return false;
            };
            
            ++filename;
            ++mask;
        };
        
        // reached end of mask string
        //  - matches if the filename string also ended
        //  - does not match if there are further characters
        //     - ending star conditions already handled above
        
        return !*filename;
    }
}

bool Windows::MatchFilenameStrict (const wchar_t * filename, const wchar_t * mask) {
    return loop (filename, mask);
}

namespace {
    wchar_t * remove (wchar_t * first, wchar_t * last, wchar_t c) {
        for (; first != last; ++first) {
            if (*first == c) {
                break;
            }
        }

        auto next = first;
        if (first != last) {
            while (++first != last) {
                if (!(*first == c)) {
                    *next = *first;
                    ++next;
                }
            }
        }
        return next;
    }
}

bool Windows::MatchNormalize (wchar_t * mask) {
    
    // empty string shall match everything
    
    if (*mask == L'\0') {
        mask [0] = L'*';
        mask [1] = L'\0';
        return true;
    }
    
    // sort various mixes of ? and * so ? go first
    //  - *? is the same as ?* but much simpler to handle
    
    unsigned int fixed = 0u;
    
    wchar_t * p = mask;
    wchar_t * q = nullptr;
    while (*p) {
        if (*p == L'*') {
            
            q = p;
            while (*q && (*q == L'*' || *q == L'?'))
                ++q;
            
            auto r = remove (p, q, L'*');
            while (r != q) {
                if (*r != L'*') {
                    *r = L'*';
                    ++fixed;
                }
                ++r;
            }
            
            p = q;
        } else
            ++p;
    }
    
    // colapse multiple stars
    
    p = mask;
    q = mask;
    
    while (*p) {
        *q++ = *p;
        
        if (*p++ == L'*') {
            while (*p == L'*') {
                ++p;
                ++fixed;
            }
        }
    }
    
    *q = L'\0';

    // fix endings: "*.*\0" replaced by "*\0"
    
    if (q - mask > 2) {
        if (q [-3] == L'*' && q [-2] == L'.' && q [-1] == L'*') {
            q [-2] = L'\0';
            ++fixed;
        }
    }
    
    return fixed;
}


bool Windows::MatchFilename (const wchar_t * filename, wchar_t * mask) {
    if (!mask || !mask[0])
        return true;
    
    Windows::MatchNormalize (mask);
    return Windows::MatchFilenameStrict (filename, mask);
}

