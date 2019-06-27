#include "b64/encode.h"

namespace base64
{

encoder::encoder(base64_eol eol, int line_length, size_t buffersize_in)
{
    base64_init_encodestate(&_state);
    reconfig(eol, line_length, buffersize_in);
}

encoder::encoder(size_t buffersize_in)
    : _buffersize(buffersize_in)
{
    base64_init_encodestate(&_state);
}

void encoder::reset_state()
{
    auto prev_state = _state;
    base64_init_encodestate(&_state);
    _state.line_length = prev_state.line_length;
    _state.eol = prev_state.eol;
}

void encoder::reconfig(base64_eol eol, int line_length, size_t buffersize_in)
{
    _buffersize = buffersize_in;
    _state.line_length = line_length;
    _state.eol = eol;
}

long encoder::encode(const char* code_in, long length_in, char* plaintext_out)
{
    return base64_encode_block(code_in, length_in, plaintext_out, &_state);
}

long encoder::encode_end(char* plaintext_out)
{
    long res = base64_encode_blockend(plaintext_out, &_state);
    reset_state();
    return res;
}

void encoder::encode(std::istream& istream_in, std::ostream& ostream_in)
{
    reset_state();
    //
    std::vector<char> output(2 * _buffersize + 3);
    std::vector<char> input(_buffersize);
    long plainlength;
    long codelength;

    do
    {
        istream_in.read(input.data(), static_cast<std::streamsize>(_buffersize));
        plainlength = istream_in.gcount();
        //
        codelength = encode(input.data(), plainlength, output.data());
        ostream_in.write(output.data(), codelength);
    }
    while (istream_in.good() && plainlength > 0);

    codelength = encode_end(output.data());
    ostream_in.write(output.data(), codelength);
    //
    reset_state();
};

std::string encode(const char* buf, size_t buf_size, base64_eol eol, int line_length)
{
    if (!buf || !buf_size)
        return "";
    base64::encoder enc(eol, line_length);
    std::string out_buf(buf_size * 2 + 3, '\0');
    long len = enc.encode(buf, static_cast<long>(buf_size), &out_buf[0]);
    len += enc.encode_end(&out_buf[0] + len);
    out_buf.resize(static_cast<size_t>(len));
    return out_buf;
}

std::string encode(const std::string &buf, base64_eol eol, int line_length)
{
    return encode(buf.data(), buf.size(), eol, line_length);
}

} // namespace base64
