#include <ap_int.h>
#include <stdint.h>

extern "C" void rtmem_migrate(ap_uint<512>* bank0,
                               ap_uint<512>* bank1,
                               ap_uint<512>* bank2,
                               uint64_t word_count,
                               uint32_t source_class,
                               uint32_t destination_class) {
#pragma HLS INTERFACE m_axi port=bank0 offset=slave bundle=bank0 max_read_burst_length=64 max_write_burst_length=64
#pragma HLS INTERFACE m_axi port=bank1 offset=slave bundle=bank1 max_read_burst_length=64 max_write_burst_length=64
#pragma HLS INTERFACE m_axi port=bank2 offset=slave bundle=bank2 max_read_burst_length=64 max_write_burst_length=64
#pragma HLS INTERFACE s_axilite port=bank0 bundle=control
#pragma HLS INTERFACE s_axilite port=bank1 bundle=control
#pragma HLS INTERFACE s_axilite port=bank2 bundle=control
#pragma HLS INTERFACE s_axilite port=word_count bundle=control
#pragma HLS INTERFACE s_axilite port=source_class bundle=control
#pragma HLS INTERFACE s_axilite port=destination_class bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    if (source_class >= 3 || destination_class >= 3 ||
        source_class == destination_class) {
        return;
    }

copy_words:
    for (uint64_t index = 0; index < word_count; ++index) {
#pragma HLS PIPELINE II=1
        ap_uint<512> word = 0;
        switch (source_class) {
            case 0:
                word = bank0[index];
                break;
            case 1:
                word = bank1[index];
                break;
            default:
                word = bank2[index];
                break;
        }

        switch (destination_class) {
            case 0:
                bank0[index] = word;
                break;
            case 1:
                bank1[index] = word;
                break;
            default:
                bank2[index] = word;
                break;
        }
    }
}
