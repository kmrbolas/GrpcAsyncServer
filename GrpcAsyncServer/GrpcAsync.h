#ifndef UNITY_GRPC_ASYNC_H
#define UNITY_GRPC_ASYNC_H
#pragma once

#include <list>
#include <functional>
#include <optional>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/async_stream.h>
#include <grpcpp/impl/codegen/sync_stream.h>
#include <grpcpp/impl/codegen/async_unary_call.h>

namespace GrpcAsync
{
	template<class Base, class Req, class Res>
	using Request_t = void(Base::*)(grpc::ServerContext*, Req*, Res*, grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*);

	template<class Base, class ReqRes>
	using RequestSingle_t = void(Base::*)(grpc::ServerContext*, ReqRes*, grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*);

	template<class Req, class Res = void>
	struct RequestFn_s { using type = std::function<void(grpc::ServerContext*, Req*, Res*, grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)>; };
	
	template<class ReqRes>
	struct RequestFn_s<ReqRes, void> { using type = std::function<void(grpc::ServerContext*, ReqRes*, grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)>; };

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
		auto try_handle(Fn fn, Args&&... args);

	protected:
		ServiceBinder* const binder;
	};

	template<class R>
	struct CoBasicServerAsyncReader : Yielder, grpc::ServerReaderInterface<R>
	{
		CoBasicServerAsyncReader(Yielder yielder, grpc::internal::AsyncReaderInterface<R>* reader_interface) : Yielder(yielder), reader(reader_interface) {  }

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

		void SendInitialMetadata() override { throw std::runtime_error("Not implemented."); }

	protected:
		grpc::internal::AsyncReaderInterface<R>* const reader;
	private:
		std::optional<R> currentMsg;
	};

	template<class W>
	struct CoBasicServerAsyncWriter : Yielder, grpc::ServerWriterInterface<W>
	{
		using grpc::ServerWriterInterface<W>::Write;
		using grpc::ServerWriterInterface<W>::WriteLast;

		CoBasicServerAsyncWriter(Yielder yielder, grpc::internal::AsyncWriterInterface<W>* writer) : Yielder(yielder), writer(writer) {  }

		void SendInitialMetadata() override { throw std::runtime_error("Not implemented."); }

		bool Write(const W& msg, grpc::WriteOptions options) final
		{
			writer->Write(msg, options, tag());
			yield();
			return ok();
		}

	protected:
		grpc::internal::AsyncWriterInterface<W>* const writer;
	};

	template<class W>
	struct CoServerAsyncWriter final : CoBasicServerAsyncWriter<W>
	{
		using grpc::ServerWriterInterface<W>::Write;
		using grpc::ServerWriterInterface<W>::WriteLast;

		CoServerAsyncWriter(Yielder yielder, grpc::ServerAsyncWriterInterface<W>* writer) : CoBasicServerAsyncWriter<W>(yielder, writer) {  }

		void SendInitialMetadata() override
		{
			get()->SendInitialMetadata(this->tag());
			this->yield();
		}

		void Finish(const grpc::Status& status) const
		{
			get()->Finish(status, this->tag());
			this->yield();
		}
		
	private:
		auto get() const { return static_cast<grpc::ServerAsyncWriterInterface<W>*>(this->writer); }
	};
	
	template<class W, class R>
	struct CoServerAsyncReader final : CoBasicServerAsyncReader<R>
	{
		using CoBasicServerAsyncReader<R>::yield;
		using CoBasicServerAsyncReader<R>::tag;

		CoServerAsyncReader(Yielder yielder, grpc::ServerAsyncReaderInterface<W, R>* reader) : CoBasicServerAsyncReader<R>(yielder, reader) {  }

		void SendInitialMetadata() override
		{
			get()->SendInitialMetadata(tag());
			yield();
		}

		void Finish(const W& msg, const grpc::Status& status)
		{
			get()->Finish(msg, status, tag());
			yield();
		}

		void FinishWithError(const grpc::Status& status)
		{
			get()->FinishWithError(status, tag());
			yield();
		}

	private:
		auto get() const { return static_cast<grpc::ServerAsyncReaderInterface<W, R>*>(this->reader); }
	};

	template<class W, class R>
	struct CoServerAsyncReaderWriter final : Yielder, grpc::ServerReaderWriterInterface<W, R>
	{
		using grpc::ServerReaderWriterInterface<W, R>::Write;
		using grpc::ServerReaderWriterInterface<W, R>::WriteLast;

		CoServerAsyncReaderWriter(ServiceBinder* binder, grpc::ServerAsyncReaderWriterInterface<W, R>* rw) : Yielder(binder), rw(rw), reader(binder, rw), writer(binder, rw) {  }

		void SendInitialMetadata() override
		{
			rw->SendInitialMetadata(tag());
			yield();
		}

		bool Read(R* msg) override { return reader.Read(msg); }

		bool NextMessageSize(uint32_t* sz) override { return reader.NextMessageSize(sz); }

		bool Write(const W& msg, grpc::WriteOptions options) override { return writer.Write(msg, options); }

		void Finish(grpc::Status status)
		{
			rw->Finish(status, tag());
			yield();
		}

	private:
		grpc::ServerAsyncReaderWriterInterface<W, R>* rw;
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
		
		grpc::ServerCompletionQueue* GetCQ() const;
		
		grpc::ServerContext ctx;

	private:
		bool Update();

		bool released;
		std::unique_ptr<ICoroutine> coroutine;
	};

	template<class Req, class Res>
	struct MethodBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<Request, grpc::ServerAsyncResponseWriter<Response>>;
		using Fn = std::function<grpc::Status(grpc::ServerContext*, const Request*, Response*)>;

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
		grpc::ServerAsyncResponseWriter<Response> responder;
		Request req;
	};

	template<class Req, class Res>
	struct ServerStreamingBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<Request, grpc::ServerAsyncWriter<Response>>;
		using Fn = std::function<grpc::Status(grpc::ServerContext*, const Request*, grpc::ServerWriterInterface<Response>*)>;

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
		grpc::ServerAsyncWriter<Response> writer;
		Request req;
	};

	template<class Req, class Res>
	struct ClientStreamingBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<grpc::ServerAsyncReader<Response, Request>>;
		using Fn = std::function<grpc::Status(grpc::ServerContext*, grpc::ServerReaderInterface<Request>*, Response*)>;

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
		grpc::ServerAsyncReader<Response, Request> reader;
	};

	template<class Req, class Res>
	struct BidirectionalStreamingBinding final : RPCBinding
	{
		using Request = Req;
		using Response = Res;

		using RequestFn = RequestFn_t<grpc::ServerAsyncReaderWriter<Response, Request>>;
		using Fn = std::function<grpc::Status(grpc::ServerContext*, grpc::ServerReaderWriterInterface<Response, Request>*)>;

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
		grpc::ServerAsyncReaderWriter<Response, Request> readerWriter;
	};

	struct ServiceBinder final
	{
		friend RPCBinding;
		friend Yielder;
		
		ServiceBinder(grpc::ServerCompletionQueue* cq) : cq(cq), tag_(nullptr), ok_(false), coroutine(nullptr), handler([](auto&fn) { return fn(); }) {  }
		
		ServiceBinder(const std::unique_ptr<grpc::ServerCompletionQueue>& cq) : ServiceBinder(cq.get()) {  }

		void Update(void* tag, bool ok);

		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, Request_t<Base, Req, grpc::ServerAsyncResponseWriter<Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new MethodBinding<Req, Res>(this, std::bind(rFn, service, _1, _2, _3, _4, _5, _6), std::bind(fn, service, _1, _2, _3));
		}
		
		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, Request_t<Base, Req, grpc::ServerAsyncWriter<Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new ServerStreamingBinding<Req, Res>(this, std::bind(rFn, service, _1, _2, _3, _4, _5, _6), std::bind(fn, service, _1, _2, _3));
		}
		
		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, RequestSingle_t<Base, grpc::ServerAsyncReader<Req, Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new ClientStreamingBinding<Req, Res>(this, std::bind(rFn, service, _1, _2, _3, _4, _5), std::bind(fn, service, _1, _2, _3));
		}

		template<class Service, class Base, class Req, class Res, class Fn>
		auto Bind(Service* service, RequestSingle_t<Base, grpc::ServerAsyncReaderWriter<Req, Res>> rFn, Fn fn)
		{
			using namespace std::placeholders;
			return new BidirectionalStreamingBinding<Req, Res>(this, std::bind(rFn, service, _1, _2, _3, _4, _5), std::bind(fn, service, _1, _2));
		}

		template<class Ex>
		ServiceBinder& AddExceptionHandler(std::function<grpc::Status(Ex&)> fn)
		{
			handler = [handler = move(handler), h = move(fn)](auto& fn)
			{
				try { return handler(fn); }
				catch (Ex& e) { return h(e); }
			};
			return *this;
		}
		
		template<class Ex, class Fn>
		ServiceBinder& AddExceptionHandler(Fn fn)
		{
			return AddExceptionHandler(std::function<grpc::Status(Ex&)>(fn));
		}

		template<class Ex>
		ServiceBinder& AddExceptionHandler(grpc::Status(*fn)(Ex&))
		{
			return AddExceptionHandler<Ex, decltype(fn)>(fn);
		}

	private:

		grpc::ServerCompletionQueue* const cq;
		std::list<std::unique_ptr<RPCBinding>> bindings;
		void* tag_;
		bool ok_;
		ICoroutine* coroutine;
		std::function<grpc::Status(const std::function<grpc::Status()>&)> handler;
	};

	inline void* Yielder::tag() const { return binder->tag_; }

	inline bool Yielder::ok() const { return binder->ok_; }

	inline void Yielder::yield() const { binder->coroutine->yield(); }

	inline grpc::ServerCompletionQueue* RPCBinding::GetCQ() const { return binder->cq; }

	template <class Fn, class... Args>
	auto Yielder::try_handle(Fn fn, Args&&... args) { return binder->handler(std::bind(fn, std::forward<Args>(args)...)); }
	
}

#endif // UNITY_GRPC_ASYNC_H