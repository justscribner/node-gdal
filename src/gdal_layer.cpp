
#include "gdal_common.hpp"
#include "gdal_layer.hpp"
#include "gdal_feature.hpp"
#include "gdal_feature_defn.hpp"
#include "gdal_field_defn.hpp"
#include "gdal_spatial_reference.hpp"
#include "gdal_dataset.hpp"
#include "collections/layer_features.hpp"
#include "collections/layer_fields.hpp"

#include <stdlib.h>
#include <sstream>

namespace node_gdal {

Persistent<FunctionTemplate> Layer::constructor;
ObjectCache<OGRLayer*> Layer::cache;

void Layer::Initialize(Handle<Object> target)
{
	HandleScope scope;

	constructor = Persistent<FunctionTemplate>::New(FunctionTemplate::New(Layer::New));
	constructor->InstanceTemplate()->SetInternalFieldCount(1);
	constructor->SetClassName(String::NewSymbol("Layer"));

	NODE_SET_PROTOTYPE_METHOD(constructor, "toString", toString);
	NODE_SET_PROTOTYPE_METHOD(constructor, "getExtent", getExtent);
	NODE_SET_PROTOTYPE_METHOD(constructor, "testCapability", testCapability);
	NODE_SET_PROTOTYPE_METHOD(constructor, "flush", syncToDisk);

	ATTR_DONT_ENUM(constructor, "ds", dsGetter, READ_ONLY_SETTER);
	ATTR(constructor, "srs", srsGetter, READ_ONLY_SETTER);
	ATTR(constructor, "features", featuresGetter, READ_ONLY_SETTER);
	ATTR(constructor, "fields", fieldsGetter, READ_ONLY_SETTER);
	ATTR(constructor, "name", nameGetter, READ_ONLY_SETTER);
	ATTR(constructor, "geomType", geomTypeGetter, READ_ONLY_SETTER);
	ATTR(constructor, "geomColumn", geomColumnGetter, READ_ONLY_SETTER);
	ATTR(constructor, "fidColumn", fidColumnGetter, READ_ONLY_SETTER);

	target->Set(String::NewSymbol("Layer"), constructor->GetFunction());
}

Layer::Layer(OGRLayer *layer)
	: ObjectWrap(),
	  this_(layer),
	  parent_ds(0),
	  is_result_set(false)
{}

Layer::Layer()
	: ObjectWrap(),
	  this_(0),
	  parent_ds(0),
	  is_result_set(false)
{
}

Layer::~Layer()
{
	dispose();
}

void Layer::dispose()
{
	if (this_) {
		if (is_result_set && parent_ds && this_) {

#ifdef VERBOSE_GC
			printf("Releasing result set [%p] from datasource [%p]\n", this_, parent_ds);
#endif

			parent_ds->ReleaseResultSet(this_);
		}

#ifdef VERBOSE_GC
		printf("Disposing layer [%p]\n", this_);
#endif
		this_ = NULL;
	}
};

Handle<Value> Layer::New(const Arguments& args)
{
	HandleScope scope;

	if (!args.IsConstructCall()) {
		return NODE_THROW("Cannot call constructor as function, you need to use 'new' keyword");
	}

	if (args[0]->IsExternal()) {
		Local<External> ext = Local<External>::Cast(args[0]);
		void* ptr = ext->Value();
		Layer *f = static_cast<Layer *>(ptr);
		f->Wrap(args.This());

		Handle<Value> features = LayerFeatures::New(args.This()); 
		args.This()->SetHiddenValue(String::NewSymbol("features_"), features); 

		Handle<Value> fields = LayerFields::New(args.This()); 
		args.This()->SetHiddenValue(String::NewSymbol("fields_"), fields); 

		return args.This();
	} else {
		return NODE_THROW("Cannot create layer directly. Create with dataset instead.");
	}

	return args.This();
}

Handle<Value> Layer::New(OGRLayer *raw, Dataset *parent)
{
	HandleScope scope;
	return scope.Close(Layer::New(raw, parent, false));
}

Handle<Value> Layer::New(OGRLayer *raw, Dataset *parent, bool result_set)
{
	HandleScope scope;

	if (!raw) {
		return v8::Null();
	}
	if (cache.has(raw)) {
		return cache.get(raw);
	}

	Layer *wrapped = new Layer(raw);
	wrapped->is_result_set = result_set;

	v8::Handle<v8::Value> ext = v8::External::New(wrapped);
	v8::Handle<v8::Object> obj = Layer::constructor->GetFunction()->NewInstance(1, &ext);

	cache.add(raw, obj);

	//add reference to datasource so datasource doesnt get GC'ed while layer is alive
	if (parent) {
		Handle<Value> ds;
		#if GDAL_VERSION_MAJOR > 2
			GDALDataset *raw_parent = parent->getDataset();
			if (Dataset::dataset_cache.has(raw_parent)) {
				ds = Dataset::dataset_cache.get(raw_parent);
			}
		#else 
			OGRDataSource *raw_parent = parent->getDatasource();
			if (Dataset::datasource_cache.has(raw_parent)) {
				ds = Dataset::datasource_cache.get(raw_parent);
			}
		#endif
		else {
			ds = Dataset::New(raw_parent); //should never happen
		}

		obj->SetHiddenValue(String::NewSymbol("ds_"), ds);
	}

	return scope.Close(obj);
}

Handle<Value> Layer::toString(const Arguments& args)
{
	HandleScope scope;

	Layer *layer = ObjectWrap::Unwrap<Layer>(args.This());
	if (!layer->this_) {
		return scope.Close(String::New("Null layer"));
	}

	std::ostringstream ss;
	ss << "Layer (" << layer->this_->GetName() << ")";

	return scope.Close(SafeString::New(ss.str().c_str()));
}

NODE_WRAPPED_METHOD_WITH_OGRERR_RESULT(Layer, syncToDisk, SyncToDisk);
NODE_WRAPPED_METHOD_WITH_RESULT_1_STRING_PARAM(Layer, testCapability, Boolean, TestCapability, "capability");

Handle<Value> Layer::getExtent(const Arguments& args)
{
	//returns object containing boundaries until complete OGREnvelope binding is built

	HandleScope scope;

	Layer *layer = ObjectWrap::Unwrap<Layer>(args.This());
	if (!layer->this_) {
		return NODE_THROW("Layer object has already been destroyed");
	}

	bool force = true;
	NODE_ARG_BOOL_OPT(0, 'force', force);

	OGREnvelope *envelope = new OGREnvelope();
	OGRErr err = layer->this_->GetExtent(envelope, force);
	if(err) {
		return NODE_THROW_OGRERR(err);
	}

	Local<Object> obj = Object::New();
	obj->Set(String::NewSymbol("minX"), Number::New(envelope->MinX));
	obj->Set(String::NewSymbol("maxX"), Number::New(envelope->MaxX));
	obj->Set(String::NewSymbol("minY"), Number::New(envelope->MinY));
	obj->Set(String::NewSymbol("maxY"), Number::New(envelope->MaxY));

	delete envelope;

	return scope.Close(obj);
}

/*
Handle<Value> Layer::getLayerDefn(const Arguments& args)
{
	HandleScope scope;

	Layer *layer = ObjectWrap::Unwrap<Layer>(args.This());

	if (!layer->this_) {
		return NODE_THROW("Layer object already destroyed");
	}

	return scope.Close(FeatureDefn::New(layer->this_->GetLayerDefn(), false));
}*/

Handle<Value> Layer::dsGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	return scope.Close(info.This()->GetHiddenValue(String::NewSymbol("ds_")));
}

Handle<Value> Layer::srsGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	Layer *layer = ObjectWrap::Unwrap<Layer>(info.This());
	if (!layer->this_) {
		return NODE_THROW("Layer object has already been destroyed");
	}
	return scope.Close(SpatialReference::New(layer->this_->GetSpatialRef(), false));
}

Handle<Value> Layer::nameGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	Layer *layer = ObjectWrap::Unwrap<Layer>(info.This());
	if (!layer->this_) {
		return NODE_THROW("Layer object has already been destroyed");
	}
	return scope.Close(SafeString::New(layer->this_->GetName()));
}

Handle<Value> Layer::geomColumnGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	Layer *layer = ObjectWrap::Unwrap<Layer>(info.This());
	if (!layer->this_) {
		return NODE_THROW("Layer object has already been destroyed");
	}
	return scope.Close(SafeString::New(layer->this_->GetGeometryColumn()));
}

Handle<Value> Layer::fidColumnGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	Layer *layer = ObjectWrap::Unwrap<Layer>(info.This());
	if (!layer->this_) {
		return NODE_THROW("Layer object has already been destroyed");
	}
	return scope.Close(SafeString::New(layer->this_->GetFIDColumn()));
}

Handle<Value> Layer::geomTypeGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	Layer *layer = ObjectWrap::Unwrap<Layer>(info.This());
	if (!layer->this_) {
		return NODE_THROW("Layer object has already been destroyed");
	}
	return scope.Close(Integer::New(layer->this_->GetGeomType()));
}

Handle<Value> Layer::featuresGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	return scope.Close(info.This()->GetHiddenValue(String::NewSymbol("features_")));
}

Handle<Value> Layer::fieldsGetter(Local<String> property, const AccessorInfo &info)
{
	HandleScope scope;
	return scope.Close(info.This()->GetHiddenValue(String::NewSymbol("fields_")));
}

} // namespace node_gdal