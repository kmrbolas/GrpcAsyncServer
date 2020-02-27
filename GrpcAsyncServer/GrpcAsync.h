#ifndef _GRPCASYNC_H
#define _GRPCASYNC_H
#pragma once

#include <list>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <exception>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/async_stream.h>
#include <grpcpp/impl/codegen/sync_stream.h>
#include <grpcpp/impl/codegen/async_unary_call.h>

namespace GrpcAsync
{
	using std::list;
	using std::vector;
	using std::function;
	using std::unique_ptr;
	using std::exception;
	using std::runtime_error;
	using std::optional;
	
	using grpc::Status;
	using grpc::CompletionQueue;
	using grpc::ServerCompletionQueue;
	using grpc::ServerContext;
	using grpc::WriteOptions;
	using grpc::ServerAsyncResponseWriter;
	using grpc::ServerReaderInterface;
	using grpc::ServerWriterInterface;
	using grpc::ServerReaderWriterInterface;
	using grpc::ServerAsyncReader;
	using grpc::ServerAsyncWriter;
	using grpc::ServerAsyncReaderWriter;
	using grpc::ServerAsyncReaderInterface;
	using grpc::ServerAsyncWriterInterface;
	using grpc::ServerAsyncReaderWriterInterface;
	using grpc::internal::AsyncReaderInterface;
	using grpc::internal::AsyncWriterInterface;
	
	template<class Base, class Req, class Res>
	using Request_t = void(Base::*)(ServerContext*, Req*, Res*, CompletionQueue*, ServerCompletionQueue*, void*);

	template<class Base, class ReqRes>
	using RequestSingle_t = void(Base::*)(ServerContext*, ReqRes*, CompletionQueue*, ServerCompletionQueue*, void*);

	template<class Req, class Res = void>
	struct RequestFn_s { using type = function<void(ServerContext*, Req*, Res*, CompletionQueue*, ServerCompletionQueue*, void*)>; };
	
	template<class ReqRes>
	struct RequestFn_s<ReqRes, void> { using type = function<void(ServerContext*, ReqRes*, CompletionQueue*, ServerCompletionQueue*, void*)>; };

	template<class Req, class Res = void>
	using RequestFn_t = typename RequestFn_s<Req, Res>::type;

	struct ICoroutine
	{
		virtual ~ICoroutine() = default;
		virtual void yield() = 0;
		virtual void invoke() = 0;
	};

	struct ServiceBinder;

	struct Yielder
	{
		Yielder(ServiceBinder* binder) : binder(binder) {  }

		void* tag() const;

		bool ok() const;

		void yield() const;

		template<class Fn, class... Args>
		decltype(auto) try_handle(Fn fn, Args&&... args);

	protected:
		ServiceBinder* const binder;
	};

	template<class R>
	struct CoBasicServerAsyncReader : Yielder, ServerReaderInterface<R>
	{
		CoBasicServerAsyncReader(Yielder yielder, AsyncReaderInterface<R>* reader_interface) : Yielder(yielder), reader(reader_interface) {  }

		bool Read(R* msg) final
		{
			if (currentMsg)
			{
				*msg = *currentMsg;
				currentMsg.reset();
				return true;
			}
			reader->Read(msg, tag());
			yield();
			return ok();
		}

		bool NextMessageSize(uint32_t* sz) final
		{
			if (!currentMsg)
			{
				currentMsg.emplace();
				if (!Read(&*currentMsg))
					return false;
			}
			*sz = (uint32_t)currentMsg->ByteSizeLong();
			return true;
		}

		void SendInitialMetadata() override { throw runtime_error("Not implemented."); }

	protected:
		AsyncReaderInterface<R>* const reader;
	private:
		optional<R> currentMsg;
	};

	template<class W>
	struct CoBasicServerAsyncWriter : Yielder, ServerWriterInterface<W>
	{
		using ServerWriterInterface<W>::Write;
		using ServerWriterInterface<W>::WriteLast;

		CoBasicServerAsyncWriter(Yielder yielder, AsyncWriterInterface<W>* writer) : Yielder(yielder), writer(writer) {  }

		void SendInitialMetadata() override { throw runtime_error("Not implemented."); }

		bool Write(const W& msg, WriteOptions options) final
		{
			writer->Write(msg, options, tag());
			yield();
			return ok();
		}

	protected:
		AsyncWriterInterface<W>* const writer;
	};

	template<class W>
	struct CoServerAsyncWriter final : CoBasicServerAsyncWriter<W>
	{
		using ServerWriterInterface<W>::Write;
		using ServerWriterInterface<W>::WriteLast;

		CoServerAsyncWriter(Yielder yielder, ServerAsyncWriterInterface<W>* writer) : CoBasicServerAsyncWriter<W>(yielder, writer) {  }

		void SendInitialMetadata() override
		{
			get()->SendInitialMetadata(this->tag());
			this->yield();
		}

		void Finish(const Status& status) const
		{
			get()->Finish(status, this->tag());
			this->yield();
		}
		
	private:
		auto get() const { return static_cast<ServerAsyncWriterInterface<W>*>(this->writer); }
	};
	
	template<class W, class R>
	struct CoServerAsyncReader final : CoBasicServerAsyncReader<R>
	{
		using CoBasicServerAsyncReader<R>::yield;
		using CoBasicServerAsyncReader<R>::tag;

		CoServerAsyncReader(Yielder yielder, ServerAsyncReaderInterface<W, R>* reader) : CoBasicServerAsyncReader<R>(yielder, reader) {  }

		void SendInitialMetadata() override
		{
			get()->SendInitialMetadata(tag());
			yield();
		}

		void Finish(const W& msg, const Status& status)
		{
			get()->Finish(msg, status, tag());
			yield();
		}

		void FinishWithError(const Status& status)
		{
			get()->FinishWithError(status, tag());
			yield();
		}

	private:
		auto get() const { return static_cast<ServerAsyncReaderInterface<W, R>*>(this->reader); }
	};

	template<class W, class R>
	struct CoServerAsyncReaderWriter final : Yielder, ServerReaderWriterInterface<W, R>
	{
		using ServerReaderWriterInterface<W, R>::Write;
		using ServerReaderWriterInterface<W, R>::WriteLast;

		CoServerAsyncReaderWriter(ServiceBinder* binder, ServerAsyncReaderWriterInterface<W, R>* rw) : Yielder(binder), rw(rw), reader(binder, rw), writer(binder, rw) {  }

		void SendInitialMetadata() override
		{
			rw->SendInitialMetadata(tag());
			yield();
		}

		bool Read(R* msg) override { return reader.Read(msg); }

		bool NextMessageSize(uint32_t* sz) override { return reader.NextMessageSize(sz); }

		bool Write(const W& msg, WriteOptions options) override { return writer.Write(msg, options); }

		void Finish(Status status)
		{
			rw->Finish(status, tag());
			yield();
		}

	private:
		ServerAsyncReaderWriterInterface<W, R>* rw;
		CoBasicServerAsyncReader<R> reader;
		CoBasicServerAsyncWriter<W> writer;
	};

	struct RPCBinding : Yielder
	{
		friend ServiceBinder;

		RPCBinding(Yielder yielder);

		virtual ~RPCBinding() = default;

	protected:

		virtual void Execute() = 0;
		
		void Release() { released = true; }
		
		ServerCompletionQueue* GetCQ() const;
		
		ServerContext ctx;

	private:
		bool Update();

		bool released;
		unique_ptr<ICoroutine> coroutine;
	};

	template<class Req, class Res>
	struct MethodBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<Request, ServerAsyncResponseWriter<Response>>;
		using Fn = function<Status(ServerContext*, const Request*, Response*)>;

		MethodBinding(Yielder yielder, RequestFn rFn, Fn fn) : RPCBinding(yielder), rFn(move(rFn)), fn(move(fn)), responder(&ctx)
		{
			this->rFn(&ctx, &req, &responder, GetCQ(), GetCQ(), this);
		}

	protected:

		void Execute() override
		{
			new MethodBinding(binder, rFn, fn);
			if (ok())
			{
				Response res;
				auto status = try_handle(fn, &ctx, &req, &res);
				if (status.ok())
					responder.Finish(res, status, this);
				else responder.FinishWithError(status, this);
				yield();
			}
			Release();
		}

		~MethodBinding() override = default;

	private:
		RequestFn rFn;
		Fn fn;
		ServerAsyncResponseWriter<Response> responder;
		Request req;
	};

	template<class Req, class Res>
	struct ServerStreamingBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<Request, ServerAsyncWriter<Response>>;
		using Fn = function<Status(ServerContext*, const Request*, ServerWriterInterface<Response>*)>;

		ServerStreamingBinding(Yielder yielder, RequestFn rFn, Fn fn) : RPCBinding(yielder), rFn(move(rFn)), fn(move(fn)), writer(&ctx)
		{
			this->rFn(&ctx, &req, &writer, GetCQ(), GetCQ(), this);
		}

	protected:

		~ServerStreamingBinding() override = default;

		void Execute() override
		{
			new ServerStreamingBinding(binder, rFn, fn);
			CoServerAsyncWriter<Response> w(binder, &writer);
			w.Finish(try_handle(fn, &ctx, &req, &w));
			Release();
		}

	private:
		RequestFn rFn;
		Fn fn;
		ServerAsyncWriter<Response> writer;
		Request req;
	};

	template<class Req, class Res>
	struct ClientStreamingBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<ServerAsyncReader<Response, Request>>;
		using Fn = function<Status(ServerContext*, ServerReaderInterface<Request>*, Response*)>;

		ClientStreamingBinding(Yielder yielder, RequestFn rFn, Fn fn) : RPCBinding(yielder), rFn(move(rFn)), fn(move(fn)), reader(&ctx)
		{
			this->rFn(&ctx, &reader, GetCQ(), GetCQ(), this);
		}

	protected:

		~ClientStreamingBinding() override = default;

		void Execute() override
		{
			new ClientStreamingBinding(binder, rFn, fn);
			Response msg;
			CoServerAsyncReader<Response, Request> r(binder, &reader);
			auto status = try_handle(fn, &ctx, &r, &msg);
			if (status.ok()) r.Finish(msg, status);
			else r.FinishWithError(status);
			Release();
		}

	private:
		RequestFn rFn;
		Fn fn;
		ServerAsyncReader<Response, Request> reader;
	};

	template<class Req, class Res>
	struct BidirectionalStreamingBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<ServerAsyncReaderWriter<Response, Request>>;
		using Fn = function<Status(ServerContext*, ServerReaderWriterInterface<Response, Request>*)>;

		BidirectionalStreamingBinding(Yielder yielder, RequestFn rFn, Fn fn) : RPCBinding(yielder), rFn(move(rFn)), fn(move(fn)), readerWriter(&ctx)
		{
			this->rFn(&ctx, &readerWriter, GetCQ(), GetCQ(), this);
		}

	protected:

		~BidirectionalStreamingBinding() override = default;

		void Execute() override
		{
			new BidirectionalStreamingBinding(binder, rFn, fn);
			CoServerAsyncReaderWriter<Response, Request> rw(binder, &readerWriter);
			rw.Finish(try_handle(fn, &ctx, &rw));
			Release();
		}

	private:
		RequestFn rFn;
		Fn fn;
		ServerAsyncReaderWriter<Response, Request> readerWriter;
	};

	struct ServiceBinder final
	{
		friend RPCBinding;
		friend Yielder;
		
		ServiceBinder(ServerCompletionQueue* cq) : cq(cq), tag_(nullptr), ok_(false), coroutine(nullptr) {  }
		
		ServiceBinder(const unique_ptr<ServerCompletionQueue>& cq) : ServiceBinder(cq.get()) {  }

		void Update(void* tag, bool ok);

		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, Request_t<Base, Req, ServerAsyncResponseWriter<Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new MethodBinding<Req, Res>(this, bind(rFn, service, _1, _2, _3, _4, _5, _6), bind(fn, service, _1, _2, _3));
		}
		
		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, Request_t<Base, Req, ServerAsyncWriter<Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new ServerStreamingBinding<Req, Res>(this, bind(rFn, service, _1, _2, _3, _4, _5, _6), bind(fn, service, _1, _2, _3));
		}
		
		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, RequestSingle_t<Base, ServerAsyncReader<Req, Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new ClientStreamingBinding<Req, Res>(this, bind(rFn, service, _1, _2, _3, _4, _5), bind(fn, service, _1, _2, _3));
		}

		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, RequestSingle_t<Base, ServerAsyncReaderWriter<Req, Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new BidirectionalStreamingBinding<Req, Res>(this, bind(rFn, service, _1, _2, _3, _4, _5), bind(fn, service, _1, _2));
		}

		template<class Ex>
		ServiceBinder& AddExceptionHandler(function<Status(Ex&)> fn)
		{
			static_assert(std::is_base_of_v<exception, Ex>, "Exception must derive from exception!");
			handlers.emplace_back([fn = move(fn)](exception& e) { return fn(dynamic_cast<Ex&>(e)); });
			return *this;
		}
		
		template<class Ex, class Fn>
		ServiceBinder& AddExceptionHandler(Fn fn)
		{
			return AddExceptionHandler(function<Status(Ex&)>(fn));
		}

		template<class Ex>
		ServiceBinder& AddExceptionHandler(Status(*fn)(Ex&))
		{
			return AddExceptionHandler<Ex, decltype(fn)>(fn);
		}

	private:

		ServerCompletionQueue* const cq;
		list<unique_ptr<RPCBinding>> bindings;
		void* tag_;
		bool ok_;
		ICoroutine* coroutine;
		vector<function<Status(exception&)>> handlers;
	};

	inline void* Yielder::tag() const { return binder->tag_; }

	inline bool Yielder::ok() const { return binder->ok_; }

	inline void Yielder::yield() const { binder->coroutine->yield(); }

	inline ServerCompletionQueue* RPCBinding::GetCQ() const { return binder->cq; }

	template <class Fn, class... Args>
	decltype(auto) Yielder::try_handle(Fn fn, Args&&... args)
	{
		try
		{
			return fn(std::forward<Args>(args)...);
		}
		catch (exception& e)
		{
			for (auto& handler : binder->handlers) try
			{
				return handler(e);
			}
			catch (std::bad_cast&) {  }
			throw;
		}
	}
	
}

#endif // UNITY_GRPC_ASYNC_H