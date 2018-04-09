/*! @file stdcpp_utils.h
 *
 *  @brief Implement some standard C++ routines using templates
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef STDCPP_UTILS_H
#define STDCPP_UTILS_H

#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <vector>
#include <functional>
#include <cctype>

#include <stdio.h>
#include <string.h>

    std::string& ltrim(std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                        std::ptr_fun<int, int>(std::isgraph)));
        return s;
    }
    
    std::string& rtrim(std::string& s)
    {
        s.erase(std::find_if(s.rbegin(), s.rend(),
                             std::ptr_fun<int, int>(std::isgraph)).base(), s.end());
        return s;
    }
    
    std::string& trim(std::string& s)
    {
        return ltrim(rtrim(s));
    }
    
    std::vector<std::string> tokenizer( const std::string& p_pcstStr, char delim )  {
        std::vector<std::string> tokens;
        std::stringstream   mySstream( p_pcstStr );
        std::string         temp;
        
        while( getline( mySstream, temp, delim ) ) {
            tokens.push_back( temp );
        }
        
        return tokens;
    }
    
    template<class T>
    bool find_key( std::map<std::string, std::string> hash, const std::string key, T& value )
    {
        std::map<std::string, std::string>::iterator it = hash.find( key.c_str() );
        if(it != hash.end())
        {
            value = atoi( it->second.c_str() );
            
            return true;
        }
        
        return false;
    }
    
    template<>
    bool find_key<std::string>( std::map<std::string, std::string> hash, const std::string key, std::string& value )
    {
        std::map<std::string, std::string>::iterator it = hash.find( key.c_str() );
        if(it != hash.end())
        {
            value = it->second;
            
            return true;
        }
        
        return false;
    }
    
    template<class T>
    bool find_key( std::map<std::string, std::string> hash, const std::string key, T& value_numerator, T& value_denominator  )
    {
        std::map<std::string, std::string>::iterator it = hash.find( key.c_str() );
        if(it != hash.end())
        {
            std::string fraction = it->second;
            std::vector<std::string> tokens = tokenizer( fraction, '/' );
            
            value_numerator = atoi( tokens[0].c_str() );
            
            if( tokens.size() == 2 )
                value_denominator = atoi( tokens[1].c_str() );
            else
                value_denominator = 1;
            
            return true;
        }
        
        return false;
    }


    template<class T>
    static T parse_field(const char *tuning_sring, const char *field_name, const char* format_specifier, T default_val, bool* found = NULL )
    {
        const char *foundStr = strstr(tuning_sring, field_name);
        T ret_data;
    
        if (foundStr)
        {
            sscanf(foundStr + strlen(field_name), format_specifier, &ret_data);
        
            if(found)
                *found = true;
        
            return ret_data;
        }
    
        if(found)
            *found = false;
    
        return default_val;
    }

#endif // STDCPP_UTILS_H
