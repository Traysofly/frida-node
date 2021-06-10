#include "runtime.h"

#include <cstring>
#include <nan.h>
#ifdef G_OS_UNIX
# include <gio/gunixsocketaddress.h>
#endif

using std::strchr;
using v8::Array;
using v8::Boolean;
using v8::Function;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Value;

namespace frida {

Runtime::Runtime(UVContext* uv_context, GLibContext* glib_context)
  : uv_context_(uv_context),
    glib_context_(glib_context),
    data_(g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL)) {
  auto isolate = Isolate::GetCurrent();
  auto global = isolate->GetCurrentContext()->Global();
  auto json_module = Local<Object>::Cast(
      Nan::Get(global, Nan::New("JSON").ToLocalChecked()).ToLocalChecked());
  auto json_stringify = Local<Function>::Cast(
      Nan::Get(json_module,
        Nan::New("stringify").ToLocalChecked()).ToLocalChecked());
  auto json_parse = Local<Function>::Cast(
      Nan::Get(json_module,
        Nan::New("parse").ToLocalChecked()).ToLocalChecked());
  json_module_.Reset(isolate, json_module);
  json_stringify_.Reset(isolate, json_stringify);
  json_parse_.Reset(isolate, json_parse);
}

Runtime::~Runtime() {
  json_parse_.Reset();
  json_stringify_.Reset();
  json_module_.Reset();

  g_hash_table_unref(data_);

  delete glib_context_;
  delete uv_context_;
}

UVContext* Runtime::GetUVContext() const {
  return uv_context_;
}

GLibContext* Runtime::GetGLibContext() const {
  return glib_context_;
}

void* Runtime::GetDataPointer(const char* id) {
  return g_hash_table_lookup(data_, id);
}

void Runtime::SetDataPointer(const char* id, void* value) {
  g_hash_table_insert(data_, const_cast<char*>(id), value);
}

Local<String> Runtime::ValueToJson(Local<Value> value) {
  auto context = Isolate::GetCurrent()->GetCurrentContext();
  auto module = Nan::New<Object>(json_module_);
  auto stringify = Nan::New<Function>(json_stringify_);
  Local<Value> argv[] = { value };
  return Local<String>::Cast(
      stringify->Call(context, module, 1, argv).ToLocalChecked());
}

Local<Value> Runtime::ValueFromJson(Local<String> json) {
  auto context = Isolate::GetCurrent()->GetCurrentContext();
  auto module = Nan::New<Object>(json_module_);
  auto parse = Nan::New<Function>(json_parse_);
  Local<Value> argv[] = { json };
  return parse->Call(context, module, 1, argv).ToLocalChecked();
}

bool Runtime::ValueToStrv(Local<Value> value, gchar*** strv, gint* length) {
  if (!value->IsArray()) {
    Nan::ThrowTypeError("Bad argument, expected an array of strings");
    return false;
  }
  auto array = Local<Array>::Cast(value);

  uint32_t n = array->Length();
  gchar** elements = g_new0(gchar*, n + 1);

  for (uint32_t i = 0; i != n; i++) {
    auto element_value = Nan::Get(array, i).ToLocalChecked();
    if (!element_value->IsString()) {
      g_strfreev(elements);
      Nan::ThrowTypeError("Bad argument, expected an array of strings only");
      return false;
    }

    Nan::Utf8String element(element_value);
    elements[i] = g_strdup(*element);
  }

  *strv = elements;
  *length = n;

  return true;
}

Local<Value> Runtime::ValueFromStrv(gchar* const* strv, gint length) {
  if (strv == NULL)
    return Nan::Null();

  auto result = Nan::New<Array>(length);
  for (gint i = 0; i != length; i++)
    Nan::Set(result, i, Nan::New(strv[i]).ToLocalChecked());
  return result;
}

bool Runtime::ValueToEnvp(Local<Value> value, gchar*** envp, gint* length) {
  auto isolate = Isolate::GetCurrent();
  auto context = isolate->GetCurrentContext();

  if (!value->IsObject()) {
    Nan::ThrowTypeError("Bad argument, expected an object");
    return false;
  }
  auto object = Local<Object>::Cast(value);

  Local<Array> names(object->GetOwnPropertyNames(context).ToLocalChecked());
  uint32_t n = names->Length();

  gchar** elements = g_new0(gchar*, n + 1);

  for (uint32_t i = 0; i != n; i++) {
    auto name = Nan::Get(names, i).ToLocalChecked();
    auto value = Nan::Get(object, name).ToLocalChecked();

    Nan::Utf8String name_str(name);
    Nan::Utf8String value_str(value);
    elements[i] = g_strconcat(*name_str, "=", *value_str, NULL);
  }

  *envp = elements;
  *length = n;

  return true;
}

Local<Value> Runtime::ValueFromEnvp(gchar* const* envp, gint length) {
  if (envp == NULL)
    return Nan::Null();

  auto result = Nan::New<Object>();
  for (gint i = 0; i != length; i++) {
    auto tokens = g_strsplit(envp[i], "=", 2);
    if (g_strv_length(tokens) == 2) {
      Nan::Set(result, Nan::New(tokens[0]).ToLocalChecked(),
          Nan::New(tokens[1]).ToLocalChecked());
    }
    g_strfreev(tokens);
  }
  return result;
}

bool Runtime::ValueToEnum(Local<Value> value, GType type, gpointer result) {
  if (!value->IsString()) {
    Nan::ThrowTypeError("Bad argument, expected a string");
    return false;
  }
  Nan::Utf8String str(value);

  bool success = false;

  auto enum_class = static_cast<GEnumClass*>(g_type_class_ref(type));

  auto enum_value = g_enum_get_value_by_nick(enum_class, *str);
  if (enum_value != NULL) {
    *((gint*) result) = enum_value->value;

    success = true;
  } else {
    auto message = g_string_sized_new(128);

    g_string_append_printf(message,
        "Enum type %s does not have a value named '%s', it only has: ",
        ClassNameFromC(g_type_name(type)), *str);

    for (guint i = 0; i != enum_class->n_values; i++) {
      if (i != 0)
        g_string_append(message, ", ");
      g_string_append_c(message, '\'');
      g_string_append(message, enum_class->values[i].value_nick);
      g_string_append_c(message, '\'');
    }

    Nan::ThrowTypeError(message->str);

    g_string_free(message, TRUE);
  }

  g_type_class_unref(enum_class);

  return success;
}

Local<String> Runtime::ValueFromEnum(gint value, GType type) {
  auto enum_class = static_cast<GEnumClass*>(g_type_class_ref(type));
  auto result = Nan::New(g_enum_get_value(enum_class, value)->value_nick)
      .ToLocalChecked();
  g_type_class_unref(enum_class);
  return result;
}

Local<Value> Runtime::ValueFromOptionsDict(GHashTable* dict) {
  auto result = Nan::New<Object>();

  GHashTableIter iter;
  gchar* key;
  GVariant* raw_value;
  g_hash_table_iter_init(&iter, dict);
  while (g_hash_table_iter_next(&iter, reinterpret_cast<gpointer*>(&key),
        reinterpret_cast<gpointer*>(&raw_value))) {
    Local<Value> value;

    if (g_variant_is_of_type(raw_value, G_VARIANT_TYPE_STRING)) {
      value = Nan::New<String>(
          g_variant_get_string(raw_value, NULL)).ToLocalChecked();
    } else if (g_variant_is_of_type(raw_value, G_VARIANT_TYPE_INT64)) {
      value = Nan::New<Number>(
          static_cast<double>(g_variant_get_int64(raw_value)));
    } else if (g_variant_is_of_type(raw_value, G_VARIANT_TYPE_BOOLEAN)) {
      value = Nan::New<Boolean>(
          static_cast<bool>(g_variant_get_boolean(raw_value)));
    } else {
      g_assert_not_reached ();
    }

    Nan::Set(result, Nan::New(key).ToLocalChecked(), value);
  }

  return result;
}

Local<Object> Runtime::ValueFromSocketAddress(GSocketAddress* address) {
  auto result = Nan::New<Object>();

  if (G_IS_INET_SOCKET_ADDRESS(address)) {
    GSocketFamily family = g_socket_address_get_family(address);
    GInetSocketAddress* sa = G_INET_SOCKET_ADDRESS(address);

    Nan::Set(result,
        Nan::New("family").ToLocalChecked(),
        Nan::New((family == G_SOCKET_FAMILY_IPV6) ? "ipv6" : "ipv4")
        .ToLocalChecked());

    gchar* host = g_inet_address_to_string(
        g_inet_socket_address_get_address(sa));
    Nan::Set(result,
        Nan::New("address").ToLocalChecked(),
        Nan::New(host).ToLocalChecked());
    g_free(host);

    Nan::Set(result, Nan::New("port").ToLocalChecked(),
        Nan::New(static_cast<uint32_t>(g_inet_socket_address_get_port(sa))));

    if (family == G_SOCKET_FAMILY_IPV6) {
      Nan::Set(result,
          Nan::New("flowlabel").ToLocalChecked(),
          Nan::New(g_inet_socket_address_get_flowinfo(sa)));
      Nan::Set(result,
          Nan::New("scopeid").ToLocalChecked(),
          Nan::New(g_inet_socket_address_get_scope_id(sa)));
    }
  }

#ifdef G_OS_UNIX
  if (G_IS_UNIX_SOCKET_ADDRESS(address)) {
    GUnixSocketAddress* sa = G_UNIX_SOCKET_ADDRESS(address);

    switch (g_unix_socket_address_get_address_type(sa)) {
      case G_UNIX_SOCKET_ADDRESS_ANONYMOUS: {
        Nan::Set(result,
            Nan::New("family").ToLocalChecked(),
            Nan::New("unix:anonymous").ToLocalChecked());

        break;
      }
      case G_UNIX_SOCKET_ADDRESS_PATH: {
        Nan::Set(result,
            Nan::New("family").ToLocalChecked(),
            Nan::New("unix:path").ToLocalChecked());

        gchar* path = g_filename_to_utf8(g_unix_socket_address_get_path(sa), -1,
            NULL, NULL, NULL);
        Nan::Set(result,
            Nan::New("path").ToLocalChecked(),
            Nan::New(path).ToLocalChecked());
        g_free(path);

        break;
      }
      case G_UNIX_SOCKET_ADDRESS_ABSTRACT:
      case G_UNIX_SOCKET_ADDRESS_ABSTRACT_PADDED: {
        Nan::Set(result,
            Nan::New("family").ToLocalChecked(),
            Nan::New("unix:abstract").ToLocalChecked());

        Nan::Set(result,
            Nan::New("path").ToLocalChecked(),
            Nan::CopyBuffer(
                g_unix_socket_address_get_path(sa),
                g_unix_socket_address_get_path_len(sa)).ToLocalChecked());

        break;
      }
      default:
        break;
    }
  }
#endif

  return result;
}

bool Runtime::ValueToCertificate(Local<Value> value,
    GTlsCertificate** certificate) {
  if (!value->IsString()) {
    Nan::ThrowTypeError("Bad argument, expected a string");
    return false;
  }
  Nan::Utf8String str(value);
  const char* cstr = *str;

  GError* error = NULL;
  if (strchr(cstr, '\n') != NULL)
    *certificate = g_tls_certificate_new_from_pem(cstr, -1, &error);
  else
    *certificate = g_tls_certificate_new_from_file(cstr, &error);

  if (error != NULL) {
    Nan::ThrowError(Nan::Error(error->message));
    g_error_free(error);
    return false;
  }

  return true;
}

const char* Runtime::ClassNameFromC(const char* cname) {
  if (g_str_has_prefix(cname, "Frida"))
    return cname + 5;

  return cname;
}

}
