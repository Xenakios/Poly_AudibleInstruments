//#include "AudibleInstruments.hpp"
#include "plugin.hpp"
#include "braids/macro_oscillator.h"
#include "braids/vco_jitter_source.h"
#include "braids/signature_waveshaper.h"

#define MAX_BRAIDS_VOICES 16

struct Braids : Module {
	enum ParamIds {
		FINE_PARAM,
		COARSE_PARAM,
		FM_PARAM,
		TIMBRE_PARAM,
		MODULATION_PARAM,
		COLOR_PARAM,
		SHAPE_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		TRIG_INPUT,
		PITCH_INPUT,
		FM_INPUT,
		TIMBRE_INPUT,
		COLOR_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT_OUTPUT,
		NUM_OUTPUTS
	};

	braids::MacroOscillator osc[MAX_BRAIDS_VOICES];
	braids::SettingsData settings[MAX_BRAIDS_VOICES];
	braids::VcoJitterSource jitter_source[MAX_BRAIDS_VOICES];
	braids::SignatureWaveshaper ws[MAX_BRAIDS_VOICES];

	dsp::SampleRateConverter<1> src[MAX_BRAIDS_VOICES];
	dsp::DoubleRingBuffer<dsp::Frame<1>, 256> outputBuffer[MAX_BRAIDS_VOICES];
	bool lastTrig[MAX_BRAIDS_VOICES];
	bool lowCpu = false;

	Braids() {
		
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
		configParam(SHAPE_PARAM, 0.0, 1.0, 0.0, "Model");
		configParam(FINE_PARAM, -1.0, 1.0, 0.0, "Fine frequency adjustment");
		configParam(COARSE_PARAM, -2.0, 2.0, 0.0, "Coarse frequency adjustment");
		configParam(FM_PARAM, -1.0, 1.0, 0.0, "FM");
		configParam(TIMBRE_PARAM, 0.0, 1.0, 0.5, "Timbre");
		configParam(MODULATION_PARAM, -1.0, 1.0, 0.0, "Modulation");
		configParam(COLOR_PARAM, 0.0, 1.0, 0.5, "Color");
		for (int i=0;i<MAX_BRAIDS_VOICES;++i)
		{
			lastTrig[i]=false;
			memset(&osc[i], 0, sizeof(osc[i]));
			osc[i].Init();
			memset(&jitter_source[i], 0, sizeof(jitter_source[i]));
			jitter_source[i].Init();
			memset(&ws[i], 0, sizeof(ws[i]));
			ws[i].Init(0x0000);
			memset(&settings[i], 0, sizeof(settings[i]));

			// List of supported settings
			settings[i].meta_modulation = 0;
			settings[i].vco_drift = 0;
			settings[i].signature = 0;
		}
		
	}

	void process(const ProcessArgs &args) override {
		// Trigger
		int polychs = std::max(inputs[PITCH_INPUT].getChannels(),1);
		for (int i=0;i<polychs;++i)
		{
			bool trig = inputs[TRIG_INPUT].getVoltage(i) >= 1.0;
			if (!lastTrig[i] && trig) {
				osc[i].Strike();
			}
			lastTrig[i] = trig;
		}
		// Render frames
		if (outputBuffer[0].empty()) {
			for (int i=0;i<polychs;++i)
			{

			float fm = params[FM_PARAM].getValue() * inputs[FM_INPUT].getVoltage();

			// Set shape
			int shape = roundf(params[SHAPE_PARAM].getValue() * braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META);
			if (settings[i].meta_modulation) {
				shape += roundf(fm / 10.0 * braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META);
			}
			settings[i].shape = clamp(shape, 0, braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META);

			// Setup oscillator from settings
			osc[i].set_shape((braids::MacroOscillatorShape) settings[i].shape);

			// Set timbre/modulation
			float timbre = params[TIMBRE_PARAM].getValue() + params[MODULATION_PARAM].getValue() * inputs[TIMBRE_INPUT].getVoltage() / 5.0;
			float modulation = params[COLOR_PARAM].getValue() + inputs[COLOR_INPUT].getVoltage() / 5.0;
			int16_t param1 = rescale(clamp(timbre, 0.0f, 1.0f), 0.0f, 1.0f, 0, INT16_MAX);
			int16_t param2 = rescale(clamp(modulation, 0.0f, 1.0f), 0.0f, 1.0f, 0, INT16_MAX);
			osc[i].set_parameters(param1, param2);

			// Set pitch
			float pitchV = inputs[PITCH_INPUT].getVoltage(i) + params[COARSE_PARAM].getValue() + params[FINE_PARAM].getValue() / 12.0;
			if (!settings[i].meta_modulation)
				pitchV += fm;
			if (lowCpu)
				pitchV += log2f(96000.f * args.sampleTime);
			int32_t pitch = (pitchV * 12.0 + 60) * 128;
			pitch += jitter_source[i].Render(settings[i].vco_drift);
			pitch = clamp(pitch, 0, 16383);
			osc[i].set_pitch(pitch);

			// TODO: add a sync input buffer (must be sample rate converted)
			uint8_t sync_buffer[24] = {};

			int16_t render_buffer[24];
			osc[i].Render(sync_buffer, render_buffer, 24);

			// Signature waveshaping, decimation (not yet supported), and bit reduction (not yet supported)
			uint16_t signature = settings[i].signature * settings[i].signature * 4095;
			for (size_t i = 0; i < 24; i++) {
				const int16_t bit_mask = 0xffff;
				int16_t sample = render_buffer[i] & bit_mask;
				int16_t warped = ws[i].Transform(sample);
				render_buffer[i] = stmlib::Mix(sample, warped, signature);
			}

			if (lowCpu) {
				for (int j = 0; j < 24; j++) {
					dsp::Frame<1> f;
					f.samples[0] = render_buffer[j] / 32768.0;
					outputBuffer[i].push(f);
				}
			}
			else {
				// Sample rate convert
				dsp::Frame<1> in[24];
				for (int j = 0; j < 24; j++) {
					in[j].samples[0] = render_buffer[j] / 32768.0;
				}
				src[i].setRates(96000, args.sampleRate);

				int inLen = 24;
				int outLen = outputBuffer[i].capacity();
				src[i].process(in, &inLen, outputBuffer[i].endData(), &outLen);
				outputBuffer[i].endIncr(outLen);
			}
			}
		}
		outputs[OUT_OUTPUT].setChannels(polychs);
		for (int i=0;i<polychs;++i)
		{
			// Output
			if (!outputBuffer[i].empty()) {
				dsp::Frame<1> f = outputBuffer[i].shift();
				outputs[OUT_OUTPUT].setVoltage(5.0 * f.samples[0],i);
			}
		}
		
	}

	json_t *dataToJson() override {
		json_t *rootJ = json_object();
		json_t *settingsJ = json_array();
		uint8_t *settingsArray = &settings[0].shape;
		for (int i = 0; i < 20; i++) {
			json_t *settingJ = json_integer(settingsArray[i]);
			json_array_insert_new(settingsJ, i, settingJ);
		}
		json_object_set_new(rootJ, "settings", settingsJ);

		json_t *lowCpuJ = json_boolean(lowCpu);
		json_object_set_new(rootJ, "lowCpu", lowCpuJ);

		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override {
		json_t *settingsJ = json_object_get(rootJ, "settings");
		if (settingsJ) {
			uint8_t *settingsArray = &settings[0].shape;
			for (int i = 0; i < 20; i++) {
				json_t *settingJ = json_array_get(settingsJ, i);
				if (settingJ)
					settingsArray[i] = json_integer_value(settingJ);
			}
		}

		json_t *lowCpuJ = json_object_get(rootJ, "lowCpu");
		if (lowCpuJ) {
			lowCpu = json_boolean_value(lowCpuJ);
		}
	}
};




static const char *algo_values[] = {
	"CSAW",
	"/\\-_",
	"//-_",
	"FOLD",
	"uuuu",
	"SUB-",
	"SUB/",
	"SYN-",
	"SYN/",
	"//x3",
	"-_x3",
	"/\\x3",
	"SIx3",
	"RING",
	"////",
	"//uu",
	"TOY*",
	"ZLPF",
	"ZPKF",
	"ZBPF",
	"ZHPF",
	"VOSM",
	"VOWL",
	"VFOF",
	"HARM",
	"FM  ",
	"FBFM",
	"WTFM",
	"PLUK",
	"BOWD",
	"BLOW",
	"FLUT",
	"BELL",
	"DRUM",
	"KICK",
	"CYMB",
	"SNAR",
	"WTBL",
	"WMAP",
	"WLIN",
	"WTx4",
	"NOIS",
	"TWNQ",
	"CLKN",
	"CLOU",
	"PRTC",
	"QPSK",
	"    ",
};

struct BraidsDisplay : TransparentWidget {
	Braids *module;
	std::shared_ptr<Font> font;

	BraidsDisplay() {
		font = APP->window->loadFont(asset::plugin(pluginInstance, "res/hdad-segment14-1.002/Segment14.ttf"));
	}

	void draw(const DrawArgs &args) override {
		int shape = module ? module->settings[0].shape : 0;

		// Background
		NVGcolor backgroundColor = nvgRGB(0x38, 0x38, 0x38);
		NVGcolor borderColor = nvgRGB(0x10, 0x10, 0x10);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.0, 0.0, box.size.x, box.size.y, 5.0);
		nvgFillColor(args.vg, backgroundColor);
		nvgFill(args.vg);
		nvgStrokeWidth(args.vg, 1.0);
		nvgStrokeColor(args.vg, borderColor);
		nvgStroke(args.vg);

		nvgFontSize(args.vg, 36);
		nvgFontFaceId(args.vg, font->handle);
		nvgTextLetterSpacing(args.vg, 2.5);

		Vec textPos = Vec(10, 48);
		NVGcolor textColor = nvgRGB(0xaf, 0xd2, 0x2c);
		nvgFillColor(args.vg, nvgTransRGBA(textColor, 16));
		nvgText(args.vg, textPos.x, textPos.y, "~~~~", NULL);
		nvgFillColor(args.vg, textColor);
		nvgText(args.vg, textPos.x, textPos.y, algo_values[shape], NULL);
	}
};


struct BraidsSettingItem : MenuItem {
	uint8_t *setting = NULL;
	uint8_t offValue = 0;
	uint8_t onValue = 1;
	void onAction(const event::Action &e) override {
		// Toggle setting
		*setting = (*setting == onValue) ? offValue : onValue;
	}
	void step() override {
		rightText = (*setting == onValue) ? "✔" : "";
		MenuItem::step();
	}
};

struct BraidsLowCpuItem : MenuItem {
	Braids *braids;
	void onAction(const event::Action &e) override {
		braids->lowCpu = !braids->lowCpu;
	}
	void step() override {
		rightText = (braids->lowCpu) ? "✔" : "";
		MenuItem::step();
	}
};


struct BraidsWidget : ModuleWidget {
	BraidsWidget(Braids *module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Braids.svg")));

		{
			BraidsDisplay *display = new BraidsDisplay();
			display->box.pos = Vec(14, 53);
			display->box.size = Vec(148, 56);
			display->module = module;
			addChild(display);
		}

		addChild(createWidget<ScrewSilver>(Vec(15, 0)));
		addChild(createWidget<ScrewSilver>(Vec(210, 0)));
		addChild(createWidget<ScrewSilver>(Vec(15, 365)));
		addChild(createWidget<ScrewSilver>(Vec(210, 365)));

		addParam(createParam<Rogan2SGray>(Vec(176, 59), module, Braids::SHAPE_PARAM));

		addParam(createParam<Rogan2PSWhite>(Vec(19, 138), module, Braids::FINE_PARAM));
		addParam(createParam<Rogan2PSWhite>(Vec(97, 138), module, Braids::COARSE_PARAM));
		addParam(createParam<Rogan2PSWhite>(Vec(176, 138), module, Braids::FM_PARAM));

		addParam(createParam<Rogan2PSGreen>(Vec(19, 217), module, Braids::TIMBRE_PARAM));
		addParam(createParam<Rogan2PSGreen>(Vec(97, 217), module, Braids::MODULATION_PARAM));
		addParam(createParam<Rogan2PSRed>(Vec(176, 217), module, Braids::COLOR_PARAM));

		addInput(createInput<PJ301MPort>(Vec(10, 316), module, Braids::TRIG_INPUT));
		addInput(createInput<PJ301MPort>(Vec(47, 316), module, Braids::PITCH_INPUT));
		addInput(createInput<PJ301MPort>(Vec(84, 316), module, Braids::FM_INPUT));
		addInput(createInput<PJ301MPort>(Vec(122, 316), module, Braids::TIMBRE_INPUT));
		addInput(createInput<PJ301MPort>(Vec(160, 316), module, Braids::COLOR_INPUT));
		addOutput(createOutput<PJ301MPort>(Vec(205, 316), module, Braids::OUT_OUTPUT));
	}

	void appendContextMenu(Menu *menu) override {
		Braids *braids = dynamic_cast<Braids*>(module);
		assert(braids);

		menu->addChild(construct<MenuLabel>());
		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "Options"));
		menu->addChild(construct<BraidsSettingItem>(&MenuItem::text, "META", &BraidsSettingItem::setting, &braids->settings[0].meta_modulation));
		menu->addChild(construct<BraidsSettingItem>(&MenuItem::text, "DRFT", &BraidsSettingItem::setting, &braids->settings[0].vco_drift, &BraidsSettingItem::onValue, 4));
		menu->addChild(construct<BraidsSettingItem>(&MenuItem::text, "SIGN", &BraidsSettingItem::setting, &braids->settings[0].signature, &BraidsSettingItem::onValue, 4));
		menu->addChild(construct<BraidsLowCpuItem>(&MenuItem::text, "Low CPU", &BraidsLowCpuItem::braids, braids));
	}
};


Model *modelBraids = createModel<Braids, BraidsWidget>("PolyBraids");
