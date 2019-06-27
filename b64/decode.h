// :mode=c++:
/*
decode.h - c++ wrapper for a base64 decoding algorithm

This is part of the libb64 project, and has been placed in the public domain.
For details, see http://sourceforge.net/projects/libb64
*/
#ifndef BASE64_DECODE_H
#define BASE64_DECODE_H

#include <iostream>
#include <vector>

namespace base64
{
	extern "C"
	{
		#include "cdecode.h"
	}

    class decoder
	{
    private:
		base64_decodestate _state;
        size_t _buffersize;

    public:
        decoder(size_t buffersize_in = 16777216)
		: _buffersize(buffersize_in)
        {
            base64_init_decodestate(&_state);
        }

        void reset_state()
        {
            base64_init_decodestate(&_state);
        }

        long decode(const char* code_in, long length_in, char* plaintext_out)
		{
			return base64_decode_block(code_in, length_in, plaintext_out, &_state);
		}

		void decode(std::istream& istream_in, std::ostream& ostream_in)
		{
			base64_init_decodestate(&_state);
			//
            std::vector<char> code(_buffersize);
            std::vector<char> plaintext(_buffersize);
            long codelength;
            long plainlength;

			do
			{
                istream_in.read(code.data(), static_cast<std::streamsize>(_buffersize));
				codelength = istream_in.gcount();
                plainlength = decode(code.data(), codelength, plaintext.data());
                ostream_in.write(plaintext.data(), plainlength);
			}
			while (istream_in.good() && codelength > 0);
            //
			base64_init_decodestate(&_state);
		}
	};

} // namespace base64



#endif // BASE64_DECODE_H

