//
// Created by pitersk on 07.06.18.
//

#include "../Headers/processing.h"
#include "../Headers/FxLMSFilter.h"
#include "../Headers/constants.h"
#include <iostream>

void secondary_path_identification(fixed_sample_type *samples_buffer, fixed_sample_type *ref_for_processing,
                                   long unsigned int buffer_length,
                                   LMSFilter<FX_FILTER_LENGTH> &sec_path_estimation_filter) {
    for (unsigned long i = 1; i < buffer_length; i += 2) {
        sample_type error_sample = signed_fixed_to_floating(samples_buffer[i]);
        sample_type reference_sample = signed_fixed_to_floating(ref_for_processing[i - 1]);
        sec_path_estimation_filter.lms_step(reference_sample, error_sample, reference_sample);
    }
}

void dc_removal(fixed_sample_type *samples_buffer, long unsigned int buffer_length) {
    static sample_type last_error_sample = 0.0f;
    static sample_type last_ref_sample = 0.0f;

    for (unsigned long i = 1; i < buffer_length; i += 2) {
        sample_type error_sample = signed_fixed_to_floating(samples_buffer[i]);
        sample_type reference_sample = signed_fixed_to_floating(samples_buffer[i - 1]);
        // error samples filtering
        sample_type new_err = error_sample + DC_REMOVAL_ALPHA * last_error_sample;
        sample_type out_err = new_err - last_error_sample;
        last_error_sample = new_err;
        samples_buffer[i] = floating_to_signed_fixed(out_err);
        // reference samples filtering
        sample_type new_ref = reference_sample + DC_REMOVAL_ALPHA * last_ref_sample;
        sample_type out_ref = new_ref - last_ref_sample;
        last_ref_sample = out_ref;
        samples_buffer[i - 1] = floating_to_signed_fixed(out_ref);
    }
}

void processing_feedforward_anc(fixed_sample_type *samples_buffer,
                                long unsigned int buffer_length) {

    static FxLMSFilter<FX_FILTER_LENGTH, FILTER_LENGTH> fxlms_filter(LMS_STEP_SIZE,
                                                                     FX_FILTER_COEFFS);
    static std::vector<sample_type> previous_reference_samples(buffer_length / 2, 0.0f);

    for (unsigned long i = 1; i < buffer_length; i += 2) {
        sample_type error_sample = INPUT_SCALING * signed_fixed_to_floating(samples_buffer[i]);
        sample_type reference_sample = INPUT_SCALING * signed_fixed_to_floating(samples_buffer[i - 1]);
        sample_type correction_sample = fxlms_filter.lms_step(previous_reference_samples.at(i / 2), error_sample);
        previous_reference_samples.at(i / 2) = reference_sample;
        fixed_sample_type fixed_correction_sample = floating_to_signed_fixed(
                correction_sample * OUTPUT_GAIN);
        samples_buffer[i] = fixed_correction_sample;
        samples_buffer[i - 1] = fixed_correction_sample;

    }
}

class SubbandValues {

public:
    typedef LMSFilter<FILTER_LENGTH> SubFilt;
    typedef std::vector<sample_type> SubVec;
    std::array<SubFilt, 4> subband_filters;
    std::array<SubVec, 4> previous_reference_samples;

    SubbandValues() : subband_filters{
            SubFilt(LMS_STEP_SIZE), SubFilt(LMS_STEP_SIZE), SubFilt(LMS_STEP_SIZE), SubFilt(LMS_STEP_SIZE)},
                      previous_reference_samples{SubVec(BUFFER_SAMPLE_SIZE / 2, 0.0f),
                                                 SubVec(BUFFER_SAMPLE_SIZE / 2, 0.0f),
                                                 SubVec(BUFFER_SAMPLE_SIZE / 2, 0.0f),
                                                 SubVec(BUFFER_SAMPLE_SIZE / 2, 0.0f)} {
    }
};

void processing_feedforward_anc_subband(sample_type *samples_buffer, long unsigned int buffer_length,
                                        unsigned int subband_num) {

    static SubbandValues subband_values;

    for (unsigned long i = 1; i < buffer_length; i += 2) {
        sample_type error_sample = INPUT_SCALING * samples_buffer[i];
        sample_type reference_sample = INPUT_SCALING * samples_buffer[i - 1];
        sample_type correction_sample = subband_values.subband_filters.at(subband_num).lms_step(
                subband_values.previous_reference_samples.at(subband_num).at(i / 2), error_sample,
                subband_values.previous_reference_samples.at(subband_num).at(i / 2));
        subband_values.previous_reference_samples.at(subband_num).at(i / 2) = reference_sample;
        samples_buffer[i] = correction_sample*OUTPUT_GAIN;
        samples_buffer[i - 1] = correction_sample*OUTPUT_GAIN;

    }
}

void processing_feedforward_anc_sec_path_modelling(fixed_sample_type *samples_buffer,
                                                   long unsigned int buffer_length) {

    static FxLMSFilter<FX_FILTER_LENGTH, FILTER_LENGTH> fxlms_filter(LMS_STEP_SIZE,
                                                                     FX_FILTER_COEFFS);
    static LMSFilter<FX_FILTER_LENGTH> sec_path_lms_estimate(SEC_PATH_LMS_STEP_SIZE,
                                                             FX_FILTER_COEFFS);
    static std::vector<sample_type> correction_samples_buffer(buffer_length / 2, 0.0f);
    static sample_type sec_path_estimation_output = 0.0f;

    for (unsigned long i = 1; i < buffer_length; i += 2) {
        // DC_REDUCTION_VALUE added to correct the dc offset in error which causes instability
        sample_type error_sample = signed_fixed_to_floating(samples_buffer[i]) + DC_REDUCTION_VALUE;
        sample_type reference_sample = signed_fixed_to_floating(samples_buffer[i - 1]);
        sample_type correction_sample = fxlms_filter.lms_step(reference_sample, error_sample);
        fixed_sample_type fixed_correction_sample = floating_to_signed_fixed(
                correction_sample * OUTPUT_GAIN);
        samples_buffer[i] = 0;
        samples_buffer[i - 1] = fixed_correction_sample;

        // Secondary path online modelling
        sec_path_estimation_output = sec_path_lms_estimate.lms_step(
                correction_samples_buffer[i / 2],
                error_sample - sec_path_estimation_output,
                correction_samples_buffer[i / 2]);
        correction_samples_buffer[i / 2] = correction_sample;
        fxlms_filter.set_s_filter_coefficient(sec_path_lms_estimate.fir_filter.get_coefficients());

    }
}

void processing_feedback_anc(fixed_sample_type *samples_buffer, long unsigned int buffer_length) {

    static FxLMSFilter<FX_FILTER_LENGTH, FILTER_LENGTH> fxlms_filter_right_channel(LMS_STEP_SIZE,
                                                                                   FX_FILTER_COEFFS);
    static FxLMSFilter<FX_FILTER_LENGTH, FILTER_LENGTH> fxlms_filter_left_channel(LMS_STEP_SIZE,
                                                                                  FX_FILTER_COEFFS);
    static FIRFilter<FX_FILTER_LENGTH> sec_path_filter_right_channel(FX_FILTER_COEFFS);
    static FIRFilter<FX_FILTER_LENGTH> sec_path_filter_left_channel(FX_FILTER_COEFFS);
    static std::vector<fixed_sample_type> output_samples(buffer_length, 0);

    for (unsigned long i = 1; i < buffer_length; i += 2) {
        sample_type error_sample_right_channel =
                signed_fixed_to_floating(samples_buffer[i]) + DC_REDUCTION_VALUE;
        sample_type error_sample_left_channel =
                signed_fixed_to_floating(samples_buffer[i - 1]) + DC_REDUCTION_VALUE;
        sample_type reference_sample_right_channel = error_sample_right_channel +
                                                     sec_path_filter_right_channel.fir_step(
                                                             signed_fixed_to_floating(
                                                                     output_samples[i]));
        sample_type reference_sample_left_channel = error_sample_left_channel +
                                                    sec_path_filter_left_channel.fir_step(
                                                            signed_fixed_to_floating(
                                                                    output_samples[i - 1]));
        sample_type correction_sample_right_channel = fxlms_filter_right_channel.lms_step(
                reference_sample_right_channel, error_sample_right_channel);
        sample_type correction_sample_left_channel = fxlms_filter_left_channel.lms_step(
                reference_sample_left_channel, error_sample_left_channel);
        output_samples[i] = floating_to_signed_fixed(correction_sample_right_channel);
        output_samples[i - 1] = floating_to_signed_fixed(correction_sample_left_channel);
        fixed_sample_type fixed_correction_sample_right_channel = floating_to_signed_fixed(
                correction_sample_right_channel * OUTPUT_GAIN);
        fixed_sample_type fixed_correction_sample_left_channel = floating_to_signed_fixed(
                correction_sample_left_channel * OUTPUT_GAIN);
        samples_buffer[i] = fixed_correction_sample_right_channel;
        samples_buffer[i - 1] = fixed_correction_sample_left_channel;

    }
}


void processing_feedback_anc_sec_path_modelling(fixed_sample_type *samples_buffer,
                                                long unsigned int buffer_length) {

    static FxLMSFilter<FX_FILTER_LENGTH, FILTER_LENGTH> fxlms_filter_right_channel(LMS_STEP_SIZE,
                                                                                   FX_FILTER_COEFFS);
    static FxLMSFilter<FX_FILTER_LENGTH, FILTER_LENGTH> fxlms_filter_left_channel(LMS_STEP_SIZE,
                                                                                  FX_FILTER_COEFFS);
    static FIRFilter<FX_FILTER_LENGTH> sec_path_filter_right_channel(FX_FILTER_COEFFS);
    static FIRFilter<FX_FILTER_LENGTH> sec_path_filter_left_channel(FX_FILTER_COEFFS);
    static std::vector<fixed_sample_type> output_samples(buffer_length, 0);

    // Online secondary path modelling static variables
    static LMSFilter<FX_FILTER_LENGTH> sec_path_lms_estimate_right_channel(SEC_PATH_LMS_STEP_SIZE,
                                                                           FX_FILTER_COEFFS);
    static std::vector<sample_type> correction_samples_buffer_right_channel(buffer_length / 2,
                                                                            0.0f);
    static sample_type sec_path_estimation_output_right_channel = 0.0f;

    static LMSFilter<FX_FILTER_LENGTH> sec_path_lms_estimate_left_channel(SEC_PATH_LMS_STEP_SIZE,
                                                                          FX_FILTER_COEFFS);
    static std::vector<sample_type> correction_samples_buffer_left_channel(buffer_length / 2, 0.0f);
    static sample_type sec_path_estimation_output_left_channel = 0.0f;

    for (unsigned long i = 1; i < buffer_length; i += 2) {
        // Feedback fxlms step right channel
        sample_type error_sample_right_channel =
                signed_fixed_to_floating(samples_buffer[i]) + DC_REDUCTION_VALUE;
        sample_type reference_sample_right_channel = error_sample_right_channel +
                                                     sec_path_filter_right_channel.fir_step(
                                                             signed_fixed_to_floating(
                                                                     output_samples[i]));
        sample_type correction_sample_right_channel = fxlms_filter_right_channel.lms_step(
                reference_sample_right_channel, error_sample_right_channel);
        output_samples[i] = floating_to_signed_fixed(correction_sample_right_channel);
        fixed_sample_type fixed_correction_sample_right_channel = floating_to_signed_fixed(
                correction_sample_right_channel * OUTPUT_GAIN);
        samples_buffer[i] = fixed_correction_sample_right_channel;
        // Feedback fxlms step left channel
        sample_type error_sample_left_channel =
                signed_fixed_to_floating(samples_buffer[i - 1]) + DC_REDUCTION_VALUE;
        sample_type reference_sample_left_channel = error_sample_left_channel +
                                                    sec_path_filter_left_channel.fir_step(
                                                            signed_fixed_to_floating(
                                                                    output_samples[i - 1]));
        sample_type correction_sample_left_channel = fxlms_filter_left_channel.lms_step(
                reference_sample_left_channel, error_sample_left_channel);
        output_samples[i - 1] = floating_to_signed_fixed(correction_sample_left_channel);
        fixed_sample_type fixed_correction_sample_left_channel = floating_to_signed_fixed(
                correction_sample_left_channel * OUTPUT_GAIN);
        samples_buffer[i - 1] = fixed_correction_sample_left_channel;

        // Online secondary path estimation right channel
        sec_path_estimation_output_right_channel = sec_path_lms_estimate_right_channel.lms_step(
                correction_samples_buffer_right_channel[i / 2],
                error_sample_right_channel - sec_path_estimation_output_right_channel,
                correction_samples_buffer_right_channel[i / 2]
        );
        correction_samples_buffer_right_channel[i / 2] = correction_sample_right_channel;
        fxlms_filter_right_channel.set_s_filter_coefficient(
                sec_path_lms_estimate_right_channel.fir_filter.get_coefficients());
        sec_path_filter_right_channel.set_coefficients(
                sec_path_lms_estimate_right_channel.fir_filter.get_coefficients());
        // Online secondary path estimation left channel
        sec_path_estimation_output_left_channel = sec_path_lms_estimate_left_channel.lms_step(
                correction_samples_buffer_left_channel[i / 2],
                error_sample_left_channel - sec_path_estimation_output_left_channel,
                correction_samples_buffer_left_channel[i / 2]
        );
        correction_samples_buffer_left_channel[i / 2] = correction_sample_left_channel;
        fxlms_filter_left_channel.set_s_filter_coefficient(
                sec_path_lms_estimate_left_channel.fir_filter.get_coefficients());
        sec_path_filter_left_channel.set_coefficients(
                sec_path_lms_estimate_left_channel.fir_filter.get_coefficients());
    }
}
