//#include "AudibleInstruments.hpp"
#include "plugin.hpp"
#pragma GCC diagnostic push
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wsuggest-override"
#endif
#include "plaits/dsp/voice.h"
#pragma GCC diagnostic pop

#define MAX_PLAITS_VOICES 16

struct Plaits : Module {
	enum ParamIds {
		MODEL1_PARAM,
		MODEL2_PARAM,
		FREQ_PARAM,
		HARMONICS_PARAM,
		TIMBRE_PARAM,
		MORPH_PARAM,
		TIMBRE_CV_PARAM,
		FREQ_CV_PARAM,
		MORPH_CV_PARAM,
		LPG_PARAM1,
		LPG_PARAM2,
		NUM_PARAMS
	};
	enum InputIds {
		ENGINE_INPUT,
		TIMBRE_INPUT,
		FREQ_INPUT,
		MORPH_INPUT,
		HARMONICS_INPUT,
		TRIGGER_INPUT,
		LEVEL_INPUT,
		NOTE_INPUT,
		LPG_PAR1_INPUT,
		LPG_PAR2_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		OUT_OUTPUT,
		AUX_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(MODEL_LIGHT, 8 * 2),
		NUM_LIGHTS
	};

	plaits::Voice voice[MAX_PLAITS_VOICES];
	plaits::Patch patch[MAX_PLAITS_VOICES] = {};
	plaits::Modulations modulations[MAX_PLAITS_VOICES] = {};
	char shared_buffer[MAX_PLAITS_VOICES][16384] = {};
	float triPhase = 0.f;

	dsp::SampleRateConverter<2> outputSrc[MAX_PLAITS_VOICES];
	dsp::DoubleRingBuffer<dsp::Frame<2>, 256> outputBuffer[MAX_PLAITS_VOICES];
	bool lowCpu = false;
	bool lpg = false;

	dsp::SchmittTrigger model1Trigger;
	dsp::SchmittTrigger model2Trigger;

	Plaits() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(MODEL1_PARAM, 0.0, 1.0, 0.0, "Model selection 1");
		configParam(MODEL2_PARAM, 0.0, 1.0, 0.0, "Model selection 2");
		configParam(FREQ_PARAM, -4.0, 4.0, 0.0, "Coarse frequency adjustment");
		configParam(HARMONICS_PARAM, 0.0, 1.0, 0.5, "Harmonics");
		configParam(TIMBRE_PARAM, 0.0, 1.0, 0.5, "Timbre");
		configParam(MORPH_PARAM, 0.0, 1.0, 0.5, "Morph");
		configParam(TIMBRE_CV_PARAM, -1.0, 1.0, 0.0, "Timbre CV");
		configParam(FREQ_CV_PARAM, -1.0, 1.0, 0.0, "Frequency CV");
		configParam(MORPH_CV_PARAM, -1.0, 1.0, 0.0, "Morph CV");
		configParam(LPG_PARAM1, 0.0, 1.0, 0.5, "LPG Colour");
		configParam(LPG_PARAM2, 0.0, 1.0, 0.5, "LPG Decay");
		for (int i=0;i<MAX_PLAITS_VOICES;++i)
		{
			stmlib::BufferAllocator allocator(shared_buffer[i], sizeof(shared_buffer[i]));
			voice[i].Init(&allocator);
		}
		onReset();
	}

	void onReset() override {
		for (int i=0;i<MAX_PLAITS_VOICES;++i)
		{
			patch[i].engine = 0;
			patch[i].lpg_colour = 0.5f;
			patch[i].decay = 0.5f;
		}

	}

	void onRandomize() override {
		for (int i=0;i<MAX_PLAITS_VOICES;++i)
			patch[i].engine = random::u32() % 16;
	}

	json_t *dataToJson() override {
		json_t *rootJ = json_object();

		json_object_set_new(rootJ, "lowCpu", json_boolean(lowCpu));
		json_object_set_new(rootJ, "model", json_integer(patch[0].engine));
		json_object_set_new(rootJ, "lpgColor", json_real(patch[0].lpg_colour));
		json_object_set_new(rootJ, "decay", json_real(patch[0].decay));

		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override {
		json_t *lowCpuJ = json_object_get(rootJ, "lowCpu");
		if (lowCpuJ)
			lowCpu = json_boolean_value(lowCpuJ);

		json_t *modelJ = json_object_get(rootJ, "model");
		if (modelJ)
			for (int i=0;i<MAX_PLAITS_VOICES;++i)
				patch[i].engine = json_integer_value(modelJ);

		json_t *lpgColorJ = json_object_get(rootJ, "lpgColor");
		if (lpgColorJ)
			for (int i=0;i<MAX_PLAITS_VOICES;++i)
				patch[i].lpg_colour = json_number_value(lpgColorJ);

		json_t *decayJ = json_object_get(rootJ, "decay");
		if (decayJ)
			for (int i=0;i<MAX_PLAITS_VOICES;++i)
				patch[i].decay = json_number_value(decayJ);
	}

	void process(const ProcessArgs &args) override {
		int numpolychs = std::max(inputs[NOTE_INPUT].getChannels(),1);
		if (outputBuffer[0].empty()) {
			const int blockSize = 12;

			// Model buttons
			if (model1Trigger.process(params[MODEL1_PARAM].getValue())) {
				for (int i=0;i<MAX_PLAITS_VOICES;++i)
				{
					if (patch[i].engine >= 8) {
						patch[i].engine -= 8;
					}
					else {
						patch[i].engine = (patch[i].engine + 1) % 8;
					}
				}
			}
			if (model2Trigger.process(params[MODEL2_PARAM].getValue())) {
				for (int i=0;i<MAX_PLAITS_VOICES;++i)
				{
					if (patch[i].engine < 8) {
						patch[i].engine += 8;
					}
					else {
						patch[i].engine = (patch[i].engine + 1) % 8 + 8;
					}
				}
			}

			// Model lights
			int activeEngine = voice[0].active_engine();
			triPhase += 2.f * args.sampleTime * blockSize;
			if (triPhase >= 1.f)
				triPhase -= 1.f;
			float tri = (triPhase < 0.5f) ? triPhase * 2.f : (1.f - triPhase) * 2.f;

			for (int i = 0; i < 8; i++) {
				lights[MODEL_LIGHT + 2*i + 0].setBrightness((activeEngine == i) ? 1.f : (patch[0].engine == i) ? tri : 0.f);
				lights[MODEL_LIGHT + 2*i + 1].setBrightness((activeEngine == i + 8) ? 1.f : (patch[0].engine == i + 8) ? tri : 0.f);
			}

			// Calculate pitch for lowCpu mode if needed
			float pitch = params[FREQ_PARAM].getValue();
			if (lowCpu)
				pitch += log2f(48000.f * args.sampleTime);
			// Update patch
			for (int i=0;i<numpolychs;++i)
			{
				patch[i].note = 60.f + pitch * 12.f;
				patch[i].harmonics = params[HARMONICS_PARAM].getValue();
				//if (!lpg) {
					patch[i].timbre = params[TIMBRE_PARAM].getValue();
					patch[i].morph = params[MORPH_PARAM].getValue();
				//}
				//else {
					float lpg_colour = params[LPG_PARAM1].getValue();
					if (inputs[LPG_PAR1_INPUT].getChannels() < 2)
						lpg_colour += inputs[LPG_PAR1_INPUT].getVoltage()/10.0f;
					else lpg_colour += inputs[LPG_PAR1_INPUT].getVoltage(i)/10.0f;
					patch[i].lpg_colour = clamp(lpg_colour,0.0f,1.0f);
					float decay = params[LPG_PARAM2].getValue();
					if (inputs[LPG_PAR2_INPUT].getChannels() < 2)
						decay += inputs[LPG_PAR2_INPUT].getVoltage()/10.0f;
					else decay += inputs[LPG_PAR2_INPUT].getVoltage(i)/10.0f;
					patch[i].decay = clamp(decay,0.0f,1.0);
				//}
				patch[i].frequency_modulation_amount = params[FREQ_CV_PARAM].getValue();
				patch[i].timbre_modulation_amount = params[TIMBRE_CV_PARAM].getValue();
				patch[i].morph_modulation_amount = params[MORPH_CV_PARAM].getValue();
				// Update modulations
				if (inputs[ENGINE_INPUT].getChannels() < 2)
					modulations[i].engine = inputs[ENGINE_INPUT].getVoltage() / 5.f;
				else
					modulations[i].engine = inputs[ENGINE_INPUT].getVoltage(i) / 5.f;
				modulations[i].note = inputs[NOTE_INPUT].getVoltage(i) * 12.f;
				modulations[i].frequency = inputs[FREQ_INPUT].getVoltage() * 6.f;
				if (inputs[HARMONICS_INPUT].getChannels() < 2)
					modulations[i].harmonics = inputs[HARMONICS_INPUT].getVoltage() / 5.f;
				else
					modulations[i].harmonics = inputs[HARMONICS_INPUT].getVoltage(i) / 5.f;
				if (inputs[TIMBRE_INPUT].getChannels() < 2)
					modulations[i].timbre = inputs[TIMBRE_INPUT].getVoltage() / 8.f;
				else
					modulations[i].timbre = inputs[TIMBRE_INPUT].getVoltage(i) / 8.f;
				if (inputs[MORPH_INPUT].getChannels() < 2)
					modulations[i].morph = inputs[MORPH_INPUT].getVoltage() / 8.f;
				else
					modulations[i].morph = inputs[MORPH_INPUT].getVoltage(i) / 8.f;
				// Triggers at around 0.7 V
				if (inputs[TRIGGER_INPUT].getChannels() < 2)
					modulations[i].trigger = inputs[TRIGGER_INPUT].getVoltage() / 3.f;
				else
					modulations[i].trigger = inputs[TRIGGER_INPUT].getVoltage(i) / 3.f;
				if (inputs[LEVEL_INPUT].getChannels() < 2)
					modulations[i].level = inputs[LEVEL_INPUT].getVoltage() / 8.f;
				else
					modulations[i].level = inputs[LEVEL_INPUT].getVoltage(i) / 8.f;

				modulations[i].frequency_patched = inputs[FREQ_INPUT].isConnected();
				modulations[i].timbre_patched = inputs[TIMBRE_INPUT].isConnected();
				modulations[i].morph_patched = inputs[MORPH_INPUT].isConnected();
				modulations[i].trigger_patched = inputs[TRIGGER_INPUT].isConnected();
				modulations[i].level_patched = inputs[LEVEL_INPUT].isConnected();
			}


			// Render frames
			for (int polych=0;polych<numpolychs;++polych)
			{
				plaits::Voice::Frame output[blockSize];
				voice[polych].Render(patch[polych], modulations[polych], output, blockSize);

				// Convert output to frames
				dsp::Frame<2> outputFrames[blockSize];
				for (int i = 0; i < blockSize; i++) {
					outputFrames[i].samples[0] = output[i].out / 32768.f;
					outputFrames[i].samples[1] = output[i].aux / 32768.f;
				}

				// Convert output
				if (lowCpu) {
					int len = std::min((int) outputBuffer[polych].capacity(), blockSize);
					memcpy(outputBuffer[polych].endData(), outputFrames, len * sizeof(dsp::Frame<2>));
					outputBuffer[polych].endIncr(len);
				}
				else {
					outputSrc[polych].setRates(48000, args.sampleRate);
					int inLen = blockSize;
					int outLen = outputBuffer[polych].capacity();
					outputSrc[polych].process(outputFrames, &inLen, outputBuffer[polych].endData(), &outLen);
					outputBuffer[polych].endIncr(outLen);
				}
			}
		}
		outputs[OUT_OUTPUT].setChannels(numpolychs);
		outputs[AUX_OUTPUT].setChannels(numpolychs);
		// Set output
		for (int i=0;i<numpolychs;++i)
		{
			if (!outputBuffer[i].empty()) {
				dsp::Frame<2> outputFrame = outputBuffer[i].shift();
				// Inverting op-amp on outputs
				outputs[OUT_OUTPUT].setVoltage(-outputFrame.samples[0] * 5.f,i);
				outputs[AUX_OUTPUT].setVoltage(-outputFrame.samples[1] * 5.f,i);
			}
		}
	}
};


static const std::string modelLabels[16] = {
	"Pair of classic waveforms",
	"Waveshaping oscillator",
	"Two operator FM",
	"Granular formant oscillator",
	"Harmonic oscillator",
	"Wavetable oscillator",
	"Chords",
	"Vowel and speech synthesis",
	"Granular cloud",
	"Filtered noise",
	"Particle noise",
	"Inharmonic string modeling",
	"Modal resonator",
	"Analog bass drum",
	"Analog snare drum",
	"Analog hi-hat",
};


struct PlaitsWidget : ModuleWidget {
	PlaitsWidget(Plaits *module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Plaits2.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParam<TL1105>(mm2px(Vec(23.32685, 14.6539)), module, Plaits::MODEL1_PARAM));
		addParam(createParam<TL1105>(mm2px(Vec(32.22764, 14.6539)), module, Plaits::MODEL2_PARAM));
		addParam(createParam<Rogan3PSWhite>(mm2px(Vec(3.1577, 20.21088)), module, Plaits::FREQ_PARAM));
		addParam(createParam<Rogan3PSWhite>(mm2px(Vec(39.3327, 20.21088)), module, Plaits::HARMONICS_PARAM));
		addParam(createParam<Rogan1PSWhite>(mm2px(Vec(4.04171, 49.6562)), module, Plaits::TIMBRE_PARAM));
		addParam(createParam<Rogan1PSWhite>(mm2px(Vec(42.71716, 49.6562)), module, Plaits::MORPH_PARAM));
		addParam(createParam<Trimpot>(mm2px(Vec(7.88712, 77.60705)), module, Plaits::TIMBRE_CV_PARAM));
		addParam(createParam<Trimpot>(mm2px(Vec(27.2245, 77.60705)), module, Plaits::FREQ_CV_PARAM));
		addParam(createParam<Trimpot>(mm2px(Vec(46.56189, 77.60705)), module, Plaits::MORPH_CV_PARAM));
		// 64, 100
		addParam(createParam<Trimpot>(mm2px(Vec(17.55581, 70.33605)), module, Plaits::LPG_PARAM1));
		addInput(createInput<PJ301MPort>(mm2px(Vec(16.52731, 80.84152)), module, Plaits::LPG_PAR1_INPUT));
		addParam(createParam<Trimpot>(mm2px(Vec(36.893195, 70.33605)), module, Plaits::LPG_PARAM2));
		addInput(createInput<PJ301MPort>(mm2px(Vec(35.864695, 80.84152)), module, Plaits::LPG_PAR2_INPUT));

		addInput(createInput<PJ301MPort>(mm2px(Vec(3.31381, 92.48067)), module, Plaits::ENGINE_INPUT));
		addInput(createInput<PJ301MPort>(mm2px(Vec(14.75983, 92.48067)), module, Plaits::TIMBRE_INPUT));
		addInput(createInput<PJ301MPort>(mm2px(Vec(26.20655, 92.48067)), module, Plaits::FREQ_INPUT));
		addInput(createInput<PJ301MPort>(mm2px(Vec(37.65257, 92.48067)), module, Plaits::MORPH_INPUT));
		addInput(createInput<PJ301MPort>(mm2px(Vec(49.0986, 92.48067)), module, Plaits::HARMONICS_INPUT));
		addInput(createInput<PJ301MPort>(mm2px(Vec(3.31381, 107.08103)), module, Plaits::TRIGGER_INPUT));
		addInput(createInput<PJ301MPort>(mm2px(Vec(14.75983, 107.08103)), module, Plaits::LEVEL_INPUT));
		addInput(createInput<PJ301MPort>(mm2px(Vec(26.20655, 107.08103)), module, Plaits::NOTE_INPUT));

		addOutput(createOutput<PJ301MPort>(mm2px(Vec(37.65257, 107.08103)), module, Plaits::OUT_OUTPUT));
		addOutput(createOutput<PJ301MPort>(mm2px(Vec(49.0986, 107.08103)), module, Plaits::AUX_OUTPUT));

		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 23.31649)), module, Plaits::MODEL_LIGHT + 0 * 2));
		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 28.71704)), module, Plaits::MODEL_LIGHT + 1 * 2));
		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 34.1162)), module, Plaits::MODEL_LIGHT + 2 * 2));
		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 39.51675)), module, Plaits::MODEL_LIGHT + 3 * 2));
		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 44.91731)), module, Plaits::MODEL_LIGHT + 4 * 2));
		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 50.31785)), module, Plaits::MODEL_LIGHT + 5 * 2));
		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 55.71771)), module, Plaits::MODEL_LIGHT + 6 * 2));
		addChild(createLight<MediumLight<GreenRedLight>>(mm2px(Vec(28.79498, 61.11827)), module, Plaits::MODEL_LIGHT + 7 * 2));
	}

	void appendContextMenu(Menu *menu) override {
		Plaits *module = dynamic_cast<Plaits*>(this->module);

		struct PlaitsLowCpuItem : MenuItem {
			Plaits *module;
			void onAction(const event::Action &e) override {
				module->lowCpu ^= true;
			}
		};

		struct PlaitsLPGItem : MenuItem {
			Plaits *module;
			void onAction(const event::Action &e) override {
				module->lpg ^= true;
			}
		};

		struct PlaitsModelItem : MenuItem {
			Plaits *module;
			int model;
			void onAction(const event::Action &e) override {
				for (int i=0;i<MAX_PLAITS_VOICES;++i)
					module->patch[i].engine = model;
			}
		};

		menu->addChild(new MenuEntry);
		PlaitsLowCpuItem *lowCpuItem = createMenuItem<PlaitsLowCpuItem>("Low CPU", CHECKMARK(module->lowCpu));
		lowCpuItem->module = module;
		menu->addChild(lowCpuItem);
		menu->addChild(new MenuEntry());
		menu->addChild(createMenuLabel("Models"));
		for (int i = 0; i < 16; i++) {
			PlaitsModelItem *modelItem = createMenuItem<PlaitsModelItem>(modelLabels[i], CHECKMARK(module->patch[0].engine == i));
			modelItem->module = module;
			modelItem->model = i;
			menu->addChild(modelItem);
		}
	}
};


Model *modelPlaits = createModel<Plaits, PlaitsWidget>("PolyPlaits");
