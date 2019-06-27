// :mode=c++:
/*
encode.h - c++ wrapper for a base64 encoding algorithm

This is part of the libb64 project, and has been placed in the public domain.
For details, see http://sourceforge.net/projects/libb64
*/
#ifndef BASE64_ENCODE_H
#define BASE64_ENCODE_H

#include <iostream>
#include <vector>

namespace base64
{
	extern "C" 
	{
		#include "cencode.h"
	}

    class encoder
	{
    private:
        base64_encodestate _state;
        size_t _buffersize;

    public:
        // typical base64 line length (multiple of 4 characters long, no more than 78 characters)
        static constexpr int im_line_length = 76;
        // intermediate buffer for stream encoding
        static constexpr size_t stream_buf_size = 16777216;

        encoder(base64_eol eol, int line_length = im_line_length, size_t buffersize_in = stream_buf_size);
        encoder(size_t buffersize_in = stream_buf_size);
        void reset_state();
        void reconfig(base64_eol eol, int line_length = im_line_length, size_t buffersize_in = stream_buf_size);
        long encode(const char* code_in, long length_in, char* plaintext_out);
        long encode_end(char* plaintext_out);
        void encode(std::istream& istream_in, std::ostream& ostream_in);
	};

    std::string encode(const char* buf, size_t buf_size, base64_eol eol = base64_eol::no_wrap, int line_length = encoder::im_line_length);
    std::string encode(const std::string &buf, base64_eol eol = base64_eol::no_wrap, int line_length = encoder::im_line_length);

    template <typename T, template <typename, typename> class Container>
    std::string encode(
            const Container<typename std::enable_if<(sizeof(T) == 1),T>::type, std::allocator<T>> &input,
            base64_eol eol = base64_eol::no_wrap,
            int line_length = encoder::im_line_length)
    {
        return encode(static_cast<const char*>(input.data()), input.size(), eol, line_length);
    }

} // namespace base64

#endif // BASE64_ENCODE_H

