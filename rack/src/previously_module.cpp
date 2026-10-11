// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack plugin: the Previously module (P0: stereo passthrough through the real engine).

#include "previously/engine.hpp"

#include "block_adapter.hpp"
#include "plugin.hpp"

#include <cstdint>
#include <optional>

namespace previously::vcv {

namespace {

/// Rack audio is ±5 V; the engine works in ±1.0 full scale.
constexpr float volts_per_full_scale = 5.0F;

class previously_module : public rack::Module {
public:
    enum input_id {
        in_left_input,
        in_right_input,
        input_count
    };

    enum output_id {
        out_left_output,
        out_right_output,
        output_count
    };

    previously_module()
    {
        config(0, input_count, output_count, 0);

        configInput(in_left_input, "Left");
        configInput(in_right_input, "Right (normalled to left)");
        configOutput(out_left_output, "Left");
        configOutput(out_right_output, "Right");

        configBypass(in_left_input, out_left_output);
        configBypass(in_right_input, out_right_output);

        start_engine(APP->engine->getSampleRate());
    }

    void process(const ProcessArgs& /*args*/) override
    {
        const float in_left  = inputs[in_left_input].getVoltage();
        const float in_right = inputs[in_right_input].getNormalVoltage(in_left);

        const stereo_frame output = adapter_.tick(*engine_,
                                                  {
                                                      .left  = in_left / volts_per_full_scale,
                                                      .right = in_right / volts_per_full_scale,
                                                  });

        outputs[out_left_output].setVoltage(output.left * volts_per_full_scale);
        outputs[out_right_output].setVoltage(output.right * volts_per_full_scale);
    }

    void onSampleRateChange(const SampleRateChangeEvent& event) override
    {
        start_engine(event.sampleRate);
    }

private:
    void start_engine(float rate_hz)
    {
        // emplace() rebuilds the engine in place: no heap allocation, safe on the audio thread.
        engine_.emplace(engine_config{
            .rate             = sample_rate{static_cast<std::uint32_t>(rate_hz)},
            .max_block_frames = block_adapter::block_frames,
        });
    }

    std::optional<previously::engine> engine_;
    block_adapter                     adapter_;
};

/// Jack centres on the panel, in millimetres from the top-left corner.
struct jack_position {
    float x_mm;
    float y_mm;
};

constexpr jack_position in_left_jack{15.0F, 96.0F};
constexpr jack_position in_right_jack{15.0F, 110.0F};
constexpr jack_position out_left_jack{35.8F, 96.0F};
constexpr jack_position out_right_jack{35.8F, 110.0F};

[[nodiscard]] rack::math::Vec to_pixels(jack_position position)
{
    return rack::mm2px(rack::math::Vec{position.x_mm, position.y_mm});
}

class previously_widget : public rack::ModuleWidget {
public:
    using jack = rack::componentlibrary::PJ301MPort;

    explicit previously_widget(previously_module* module)
    {
        setModule(module);
        setPanel(rack::createPanel(rack::asset::plugin(pluginInstance, "res/previously.svg")));

        addInput(rack::createInputCentered<jack>(to_pixels(in_left_jack),
                                                 module,
                                                 previously_module::in_left_input));
        addInput(rack::createInputCentered<jack>(to_pixels(in_right_jack),
                                                 module,
                                                 previously_module::in_right_input));
        addOutput(rack::createOutputCentered<jack>(to_pixels(out_left_jack),
                                                   module,
                                                   previously_module::out_left_output));
        addOutput(rack::createOutputCentered<jack>(to_pixels(out_right_jack),
                                                   module,
                                                   previously_module::out_right_output));
    }
};

}  // namespace

}  // namespace previously::vcv

// NOLINTNEXTLINE(*-avoid-non-const-global-variables): Rack's model registry
rack::Model* model_previously =
    rack::createModel<previously::vcv::previously_module, previously::vcv::previously_widget>(
        "Previously");
