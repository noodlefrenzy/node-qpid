#define BUILDING_NODE_EXTENSION
#include <node.h>
#include <iostream>
#include <vector>
#include "messenger.h"
#include "async.h"

using namespace v8;
using namespace node;
using namespace std;

Messenger::Messenger() { };

Messenger::~Messenger() { };

Persistent<FunctionTemplate> Messenger::constructor_template;

void Messenger::Init(Handle<Object> target) {
  HandleScope scope;

  Local<FunctionTemplate> t = FunctionTemplate::New(New);

  constructor_template = Persistent<FunctionTemplate>::New(t);
  constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
  constructor_template->SetClassName(String::NewSymbol("Messenger"));

  NODE_SET_PROTOTYPE_METHOD(constructor_template, "send", Send);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "subscribe", Subscribe);
  NODE_SET_PROTOTYPE_METHOD(constructor_template, "listen", Listen);

  target->Set(String::NewSymbol("Messenger"),
    constructor_template->GetFunction());
 
}

Handle<Value> Messenger::New(const Arguments& args) {
  HandleScope scope;

  Messenger* msgr = new Messenger();
  msgr->messenger = pn_messenger(NULL);
  msgr->receiver = pn_messenger(NULL);
  msgr->listening = false;

  // Does this work?
  // (not like this, no)
  if (!args[0]->IsUndefined()) {
    Subscribe(args);
  }

  msgr->Wrap(args.This());

  return args.This();
}

Handle<Value> Messenger::Subscribe(const Arguments& args) {
  HandleScope scope;

  Messenger* msgr = ObjectWrap::Unwrap<Messenger>(args.This());

  REQUIRE_ARGUMENT_STRING(0, address);
  OPTIONAL_ARGUMENT_FUNCTION(1, callback);

  // This assumes that a messenger can only subscribe to one address at a time
  // (which may not be true)
  msgr->address = *address;

  SubscribeBaton* baton = new SubscribeBaton(msgr, callback, *address);
  
  cerr << "Messenger::Subscribe: Subscribing to " << baton->address << "\n";

  Work_BeginSubscribe(baton);

  return args.This();
  
}

void Messenger::Work_BeginSubscribe(Baton* baton) {
  int status = uv_queue_work(uv_default_loop(),
    &baton->request, Work_Subscribe, Work_AfterSubscribe);

  assert(status == 0);

}

void Messenger::Work_Subscribe(uv_work_t* req) {

  SubscribeBaton* baton = static_cast<SubscribeBaton*>(req->data);

  cerr << "Work_Subscribe: Subscribing to " << baton->address << "\n";
  pn_messenger_subscribe(baton->msgr->receiver, baton->address.c_str());

}

void Messenger::Work_AfterSubscribe(uv_work_t* req) {

  SubscribeBaton* baton = static_cast<SubscribeBaton*>(req->data);

  if (baton->error_code > 0) {
    /* Local<Value> err = Exception::Error(
    String::New(baton->error_message.c_str()));
    Local<Value> argv[2] = { Local<Value>::New(String::New("error")), err };
    MakeCallback(baton->obj, "emit", 2, argv); */
  } else {
    cerr << "Work_AfterSubscribe: Emitting 'subscribed' event\n";
    Local<Value> args[] = { String::New("subscribed") };
    // MakeCallback(baton->msgr, "emit", 1, argv);
    EMIT_EVENT(baton->msgr->handle_, 1, args);
  }

  delete baton;

}

Handle<Value> Messenger::Send(const Arguments& args) {
  HandleScope scope;
  
  Messenger* msgr = ObjectWrap::Unwrap<Messenger>(args.This());

  REQUIRE_ARGUMENT_STRING(0, msg);
  OPTIONAL_ARGUMENT_FUNCTION(1, callback);

  SendBaton* baton = new SendBaton(msgr, callback, *msg);
  
  Work_BeginSend(baton);

  return Undefined();
    
}

void Messenger::Work_BeginSend(Baton* baton) {
  int status = uv_queue_work(uv_default_loop(),
    &baton->request, Work_Send, Work_AfterSend);

  assert(status == 0);

}

void Messenger::Work_Send(uv_work_t* req) {

  int ret = 0;
  SendBaton* baton = static_cast<SendBaton*>(req->data);
  pn_messenger_t* messenger = baton->msgr->messenger;

  pn_message_t* message = pn_message();

  // For now, it defaults to sending a message to the (one) subscribed address
  pn_message_set_address(message, baton->msgr->address.c_str());
  pn_data_t* body = pn_message_body(message);

  pn_data_put_string(body, pn_bytes(baton->msgtext.size(), const_cast<char*>(baton->msgtext.c_str())));

  assert(!pn_messenger_put(messenger, message));
  baton->tracker = pn_messenger_outgoing_tracker(messenger);
  cerr << "Work_Send: Put message '" << pn_data_get_string(pn_message_body(message)).start << "' (return value: " << ret << ", tracker: " << baton->tracker << ", status: " << pn_messenger_status(messenger,baton->tracker) << ", outgoing: " << pn_messenger_outgoing(messenger) << ")\n";
  
  assert(!pn_messenger_start(messenger));

  assert(!pn_messenger_send(messenger));
  cerr << "Work_Send: Sent message (return value: " << ret << ", tracker: " << baton->tracker << ", status: " << pn_messenger_status(messenger,baton->tracker) << ", outgoing: " << pn_messenger_outgoing(messenger) << "\n";
  
  pn_messenger_stop(messenger);

  pn_message_free(message);

  // Where to put this?!?
  // pn_messenger_free(messenger);
}

void Messenger::Work_AfterSend(uv_work_t* req) {
  HandleScope scope;
  SendBaton* baton = static_cast<SendBaton*>(req->data);

  if (baton->error_code > 0) {
    Local<Value> err = Exception::Error(String::New(baton->error_message.c_str()));
    Local<Value> argv[] = { err };
    baton->callback->Call(Context::GetCurrent()->Global(), 1, argv);
  } else {
    cerr << "Work_AfterSend: Invoking callback on success (tracker: " << baton->tracker << ")\n";
    Local<Value> argv[] = {};
    baton->callback->Call(Context::GetCurrent()->Global(), 0, argv);
  }

  delete baton;

}

Handle<Value> Messenger::Listen(const Arguments& args) {
  HandleScope scope;

  Messenger* msgr = ObjectWrap::Unwrap<Messenger>(args.This());

  cerr << "Messenger::Listen: About to check listening (which is " << msgr->listening << ")\n";

  if (!msgr->listening) {

    Local<Function> emitter = Local<Function>::Cast((msgr->handle_)->Get(String::NewSymbol("emit")));
    ListenBaton* baton = new ListenBaton(msgr, emitter);

    cerr << "Messenger::Listen: About to BeginListen\n";
    Work_BeginListen(baton);

  }

  return Undefined();

}

void Messenger::Work_BeginListen(Baton *baton) {

  ListenBaton* listen_baton = static_cast<ListenBaton*>(baton);
  listen_baton->async = new Async(listen_baton->msgr, AsyncListen);
  listen_baton->async->emitter = Persistent<Function>::New(listen_baton->callback);

  listen_baton->msgr->listening = true;

  cerr << "Work_BeginListen: About to uv_queue_work\n";
  
  int status = uv_queue_work(uv_default_loop(),
    &baton->request, Work_Listen, Work_AfterListen);

  assert(status == 0);

}

void Messenger::CloseEmitter(uv_handle_t* handle) {

  assert(handle != NULL);
  assert(handle->data != NULL);
  Async* async = static_cast<Async*>(handle->data);
  delete async;
  handle->data = NULL;

}

void Messenger::Work_Listen(uv_work_t* req) {

  ListenBaton* baton = static_cast<ListenBaton*>(req->data);
  pn_messenger_t* receiver = baton->msgr->receiver;
  Async* async = baton->async;

  sleep(5);

  while (baton->msgr->listening) {

    cerr << "Work_Listen: About to block on recv\n";

    pn_messenger_recv(receiver, 1024);

    cerr << "Work_Listen: Leaving blocking recv (incoming = " << pn_messenger_incoming(receiver) << ", error = " << pn_messenger_error(receiver) << ")\n";

    while(pn_messenger_incoming(receiver)) {

      cerr << "Work_Listen: Iterating over incoming messages\n";

      pn_message_t* message;
      pn_messenger_get(receiver, message);

      NODE_CPROTON_MUTEX_LOCK(&async->mutex)
      async->data.push_back(message);
      NODE_CPROTON_MUTEX_UNLOCK(&async->mutex)

      uv_async_send(&async->watcher);

    } 

  }

  async->completed = true;
  uv_async_send(&async->watcher);

}

void Messenger::AsyncListen(uv_async_t* handle, int status) {
  HandleScope scope;
  Async* async = static_cast<Async*>(handle->data);

  while (true) {

    Messages messages;
    NODE_CPROTON_MUTEX_LOCK(&async->mutex)
    messages.swap(async->data);
    NODE_CPROTON_MUTEX_UNLOCK(&async->mutex)

    if (messages.empty()) {
      break;
    }

    Local<Value> argv[1];

    Messages::const_iterator it = messages.begin();
    Messages::const_iterator end = messages.end();
    for (int i = 0; it < end; it++, i++) {

      argv[0] = String::NewSymbol("Message!");
      TRY_CATCH_CALL(async->msgr->handle_, async->emitter, 1, argv)
      delete *it;

    }

  }

  if (async->completed) {
    uv_close((uv_handle_t*)handle, CloseEmitter);
  }

}

void Messenger::Work_AfterListen(uv_work_t* req) {

  HandleScope scope;
  SendBaton* baton = static_cast<SendBaton*>(req->data);

  cerr << "Work_AfterListen:  cleaning up\n";

  delete baton;

}

Handle<Value> Messenger::Stop(const Arguments& args) {
  HandleScope scope;

  OPTIONAL_ARGUMENT_FUNCTION(0, callback);

  Messenger* msgr = ObjectWrap::Unwrap<Messenger>(args.This());

  Baton* baton = new Baton(msgr, callback);

  Work_BeginStop(baton);

  return Undefined();

}

void Messenger::Work_BeginStop(Baton *baton) {

  int status = uv_queue_work(uv_default_loop(),
    &baton->request, Work_Stop, Work_AfterStop);

  assert(status == 0);

}

void Messenger::Work_Stop(uv_work_t* req) {

  // Set some flag to indicate should no longer listen
  // Call driver "wakeup" function to stop blocking messenger receive function call

}

void Messenger::Work_AfterStop(uv_work_t* req) {

}
