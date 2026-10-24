// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack plugin: the Previously module. Audio passthrough through the real engine, which
// records its input onto a tape; clock and reset inputs, the tracked grid on clock out, beat, bar
// and clock-state lights, and the screen (DESIGN.md §9.3).

#include "previously/clock_events.hpp"
#include "previously/display.hpp"
#include "previously/engine.hpp"
#include "previously/framebuffer.hpp"
#include "previously/ui_model.hpp"

#include "block_adapter.hpp"
#include "plugin.hpp"
#include "screen.hpp"
#include "tape.hpp"

#include <cstdint>
#include <optional>

namespace previously::vcv {

namespace {

/// Rack audio is ±5 V; the engine works in ±1.0 full scale.
constexpr float volts_per_full_scale = 5.0F;

/// How long the beat and bar lights stay lit after each pulse.
constexpr float flash_seconds = 0.05F;

class previously_module : public rack::Module {
public:
    enum input_id {
        in_left_input,
        in_right_input,
        clock_input,
        reset_input,
        input_count
    };

    enum output_id {
        out_left_output,
        out_right_output,
        clock_output,
        output_count
    };

    enum light_id {
        beat_light,
        bar_light,
        clock_state_light,  // two LEDs: green, then red (both lit reads as amber)
        clock_state_light_red,
        light_count
    };

    previously_module()
    {
        config(0, input_count, output_count, light_count);

        configInput(in_left_input, "Left");
        configInput(in_right_input, "Right (normalled to left)");
        configInput(clock_input, "Clock (4 PPQN)");
        configInput(reset_input, "Reset");
        configOutput(out_left_output, "Left");
        configOutput(out_right_output, "Right");
        configOutput(clock_output, "Clock (the tracked grid; keeps running while freewheeling)");
        configLight(beat_light, "Beat");
        configLight(bar_light, "Bar");
        configLight(clock_state_light, "Clock: green locked, amber freewheeling, red acquiring");

        configBypass(in_left_input, out_left_output);
        configBypass(in_right_input, out_right_output);

        start(APP->engine->getSampleRate());
    }

    void process(const ProcessArgs& args) override
    {
        const float in_left  = inputs[in_left_input].getVoltage();
        const float in_right = inputs[in_right_input].getNormalVoltage(in_left);

        const tick_output output =
            adapter_->tick(*engine_,
                           {
                               .left  = in_left / volts_per_full_scale,
                               .right = in_right / volts_per_full_scale,
                           },
                           {
                               .clock_volts = inputs[clock_input].getVoltage(),
                               .reset_volts = inputs[reset_input].getVoltage(),
                           });

        outputs[out_left_output].setVoltage(output.audio.left * volts_per_full_scale);
        outputs[out_right_output].setVoltage(output.audio.right * volts_per_full_scale);
        outputs[clock_output].setVoltage(output.clock_out_volts);

        if (output.block_finished) {
            show_clock(engine_->clock());
            screen_.after_block(*engine_);
        }

        lights[beat_light].setBrightness(beat_flash_.process(args.sampleTime) ? 1.0F : 0.0F);
        lights[bar_light].setBrightness(bar_flash_.process(args.sampleTime) ? 1.0F : 0.0F);
    }

    void onSampleRateChange(const SampleRateChangeEvent& event) override
    {
        start(event.sampleRate);
    }

    /// The models for the screen. Its widget takes them on the UI thread.
    [[nodiscard]] screen_feed& screen() noexcept
    {
        return screen_;
    }

private:
    void start(float rate_hz)
    {
        // emplace() rebuilds in place: no heap allocation, safe on the audio thread. The tape was
        // allocated with the module, and every rate records into it (tape.hpp).
        const sample_rate rate{static_cast<std::uint32_t>(rate_hz)};

        engine_.emplace(
            engine_config{
                .rate             = rate,
                .max_block_frames = block_adapter::block_frames,
            },
            tape_.for_rate(rate));
        adapter_.emplace(rate);
        screen_.restart();
    }

    void show_clock(const clock_report& report)
    {
        for (std::size_t i = 0; i < report.pulse_count; ++i) {
            if (report.pulses[i].starts_beat) {
                beat_flash_.trigger(flash_seconds);
            }
            if (report.pulses[i].starts_bar) {
                bar_flash_.trigger(flash_seconds);
            }
        }

        // A reset can make an already-reported pulse the start of bar 0.
        if (report.bar_restarted) {
            bar_flash_.trigger(flash_seconds);
        }

        const bool green =
            report.state == clock_state::locked || report.state == clock_state::freewheeling;
        const bool red =
            report.state == clock_state::acquiring || report.state == clock_state::freewheeling;

        lights[clock_state_light].setBrightness(green ? 1.0F : 0.0F);
        lights[clock_state_light_red].setBrightness(red ? 1.0F : 0.0F);
    }

    tape                              tape_;  // declared before the engine, which records into it
    std::optional<previously::engine> engine_;
    std::optional<block_adapter>      adapter_;
    rack::dsp::PulseGenerator         beat_flash_;
    rack::dsp::PulseGenerator         bar_flash_;
    screen_feed                       screen_;  // the module's, so it outlives each engine
};

/// Jack centres on the panel, in millimetres from the top-left corner.
struct jack_position {
    float x_mm;
    float y_mm;
};

constexpr jack_position beat_led{12.0F, 22.0F};
constexpr jack_position bar_led{25.4F, 22.0F};
constexpr jack_position clock_state_led{38.8F, 22.0F};
constexpr jack_position clock_jack{15.0F, 42.0F};
constexpr jack_position reset_jack{35.8F, 42.0F};
constexpr jack_position clock_out_jack{35.8F, 60.0F};
constexpr jack_position in_left_jack{15.0F, 99.0F};
constexpr jack_position in_right_jack{15.0F, 113.0F};
constexpr jack_position out_left_jack{35.8F, 99.0F};
constexpr jack_position out_right_jack{35.8F, 113.0F};

/// The screen's pixels: a 1.54" SSD1309's active area, centred in the panel's viewing window
/// between the clock and audio boxes, which is 1 mm wider all round (DESIGN.md §9.3).
constexpr jack_position screen_centre{25.4F, 77.5F};
constexpr float         screen_width_mm  = 35.05F;
constexpr float         screen_height_mm = 17.51F;

[[nodiscard]] rack::math::Vec to_pixels(jack_position position)
{
    return rack::mm2px(rack::math::Vec{position.x_mm, position.y_mm});
}

/// The screen: the module's newest model, drawn into a framebuffer and painted on the light
/// layer, so it stays lit when the room is dimmed. In the module browser there is no module, and
/// it shows an empty model.
class screen_widget : public rack::widget::Widget {
public:
    explicit screen_widget(previously_module* instance) noexcept
        : module_{instance}
    {
        box.size = rack::mm2px(rack::math::Vec{screen_width_mm, screen_height_mm});
        box.pos  = to_pixels(screen_centre).minus(box.size.div(2.0F));
    }

    void drawLayer(const DrawArgs& args, int layer) override
    {
        if (layer == 1) {
            refresh();
            paint(args.vg);
        }

        Widget::drawLayer(args, layer);
    }

private:
    /// Draws the newest model, if there is one the framebuffer does not show yet.
    void refresh()
    {
        if (module_ != nullptr && module_->screen().take()) {
            previously::draw(module_->screen().model(), pixels_);
            drawn_ = true;
        }

        if (!drawn_) {
            previously::draw(ui_model{}, pixels_);
            drawn_ = true;
        }
    }

    /// A rectangle for each column's run of lit pixels. Anti-aliasing is off, so the edges where
    /// runs in neighbouring columns meet leave no faint seams.
    void paint(NVGcontext* vg) const
    {
        const float pixel_width  = box.size.x / static_cast<float>(framebuffer::width);
        const float pixel_height = box.size.y / static_cast<float>(framebuffer::height);

        nvgShapeAntiAlias(vg, 0);
        nvgBeginPath(vg);

        for_each_run(pixels_, [vg, pixel_width, pixel_height](pixel_run run) {
            nvgRect(vg,
                    static_cast<float>(run.x) * pixel_width,
                    static_cast<float>(run.top) * pixel_height,
                    pixel_width,
                    static_cast<float>(run.bottom - run.top) * pixel_height);
        });

        nvgFillColor(vg, nvgRGB(0xE8, 0xF1, 0xFF));
        nvgFill(vg);
    }

    previously_module* module_;
    framebuffer        pixels_;
    bool               drawn_{false};
};

class previously_widget : public rack::ModuleWidget {
public:
    using jack = rack::componentlibrary::PJ301MPort;

    explicit previously_widget(previously_module* instance)
    {
        // "instance", not "module": ModuleWidget already has a member named module.
        setModule(instance);
        setPanel(rack::createPanel(rack::asset::plugin(pluginInstance, "res/previously.svg")));

        addInput(rack::createInputCentered<jack>(to_pixels(in_left_jack),
                                                 instance,
                                                 previously_module::in_left_input));
        addInput(rack::createInputCentered<jack>(to_pixels(in_right_jack),
                                                 instance,
                                                 previously_module::in_right_input));
        addOutput(rack::createOutputCentered<jack>(to_pixels(out_left_jack),
                                                   instance,
                                                   previously_module::out_left_output));
        addOutput(rack::createOutputCentered<jack>(to_pixels(out_right_jack),
                                                   instance,
                                                   previously_module::out_right_output));

        addInput(rack::createInputCentered<jack>(to_pixels(clock_jack),
                                                 instance,
                                                 previously_module::clock_input));
        addInput(rack::createInputCentered<jack>(to_pixels(reset_jack),
                                                 instance,
                                                 previously_module::reset_input));
        addOutput(rack::createOutputCentered<jack>(to_pixels(clock_out_jack),
                                                   instance,
                                                   previously_module::clock_output));

        using rack::componentlibrary::GreenRedLight;
        using rack::componentlibrary::MediumLight;
        using rack::componentlibrary::RedLight;
        using rack::componentlibrary::YellowLight;

        addChild(
            rack::createLightCentered<MediumLight<YellowLight>>(to_pixels(beat_led),
                                                                instance,
                                                                previously_module::beat_light));
        addChild(rack::createLightCentered<MediumLight<RedLight>>(to_pixels(bar_led),
                                                                  instance,
                                                                  previously_module::bar_light));
        addChild(rack::createLightCentered<MediumLight<GreenRedLight>>(
            to_pixels(clock_state_led),
            instance,
            previously_module::clock_state_light));

        addChild(new screen_widget{instance});  // Rack deletes a widget with its parent
    }
};

}  // namespace

}  // namespace previously::vcv

// NOLINTNEXTLINE(*-avoid-non-const-global-variables): Rack's model registry
rack::Model* model_previously =
    rack::createModel<previously::vcv::previously_module, previously::vcv::previously_widget>(
        "Previously");
