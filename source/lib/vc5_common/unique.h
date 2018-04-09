/*! @file unique.h
 *
 *  @brief Declaration of data structures for the unique image identifier.
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

#ifndef UNIQUE_H
#define UNIQUE_H

// Basic UMID label using the UUID method

static const uint8_t UMID_label[] = {
    0x06, 0x0A, 0x2B, 0x34, 0x01, 0x01, 0x01, 0x05, 0x01, 0x01, 0x01, 0x20,
};

// Length of the UMID (in bytes)
#define UMID_size 32

// Length of the unique identifier chunk payload (in segments)
#define UMID_length (UMID_size/sizeof(SEGMENT))

// Length of the image sequence number (in bytes)
#define sequence_number_size sizeof(uint32_t)

// Length of the image sequence number (in segments)
#define sequence_number_length (sequence_number_size/sizeof(SEGMENT))

#endif // UNIQUE_H
