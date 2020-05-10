#include "plugin.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
	pluginInstance = p;
	p->addModel(modelPlaits);
	p->addModel(modelBraids);
	p->addModel(modelMarbles);
	p->addModel(modelClouds);
}
