#ifndef WINDOWS_MATCHFILENAME_HPP
#define WINDOWS_MATCHFILENAME_HPP

/* Windows MatchFilename 
// Windows_MatchFilename.hpp
//
// Author: Jan Ringos, http://Tringi.MX-3.cz, jan@ringos.cz
// Version: 0.1
//
// Changelog:
//      16.04.2013 - initial version
*/

#include <windows.h>

namespace Windows {
    
    // MatchFilename
    //  - matches provided filename against DOS filename mask
    //     - wildcards supported: * - matches any number of characters
    //                            ? - matches exactly one any character
    //  - normalizes the mask first
    //  - returns true if the filename matches, false otherwise
    
    bool MatchFilename (const wchar_t * filename, wchar_t * mask);

    // MatchFilenameStrict
    //  - matches provided filename against normalized DOS filename mask
    //     - wildcards supported: * - matches any number of characters
    //                            ? - matches exactly one any character
    //  - NOTE: will NOT work reliably over not-normalized mask
    //  - returns true if the filename matches, false otherwise
    
    bool MatchFilenameStrict (const wchar_t * filename, const wchar_t * mask);

    // MatchNormalize
    //  - fixes unsupported or overcomplicated 'mask' constructions
    //     - sorts mixes of stars and question marks so the later go first
    //     - collapses series of stars into one
    //     - shortens ending *.* to single star
    //  - mask is required to be at least 2 characters long
    //     - empty strings are normalized to L"*"
    //  - returns true if 'mask' was fixed or false if already proper

    bool MatchNormalize (wchar_t * mask);
    
};

#endif

