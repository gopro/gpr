/*! @file gpr_log.h
 *
 *  @brief Declatation of functions used for logging
 *
 *  @version 1.0.0
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

#ifndef GPR_LOG_H
#define GPR_LOG_H

#ifdef __cplusplus
extern "C" {
#endif
        
    bool LogInit();

    #ifndef LogPrint
    int  LogPrint(const char* format, ... );
    #endif

    bool LogUninit();

    #define TIMESTAMP(x, y) TIMESTAMP_##y(x)
    
    #if GPR_TIMING == 0
        #define TIMESTAMP_3(x)
        #define TIMESTAMP_2(x)
        #define TIMESTAMP_1(x)
    #elif GPR_TIMING == 1
        #define TIMESTAMP_3(x)
        #define TIMESTAMP_2(x)
        #define TIMESTAMP_1(x) LogPrint("%s %s() %s (line %d)",  x, __FUNCTION__, __FILE__, __LINE__);
    #elif GPR_TIMING == 2
        #define TIMESTAMP_3(x)
        #define TIMESTAMP_2(x) LogPrint("%s %s() %s (line %d)",  x, __FUNCTION__, __FILE__, __LINE__);
        #define TIMESTAMP_1(x) LogPrint("%s %s() %s (line %d)",  x, __FUNCTION__, __FILE__, __LINE__);
    #elif GPR_TIMING == 3
        #define TIMESTAMP_3(x) LogPrint("%s %s() %s (line %d)",  x, __FUNCTION__, __FILE__, __LINE__);
        #define TIMESTAMP_2(x) LogPrint("%s %s() %s (line %d)",  x, __FUNCTION__, __FILE__, __LINE__);
        #define TIMESTAMP_1(x) LogPrint("%s %s() %s (line %d)",  x, __FUNCTION__, __FILE__, __LINE__);
    #endif
        
#ifdef __cplusplus
}
#endif

#endif // GPR_LOG_H
