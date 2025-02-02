/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/async_requests_sender.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

// Maximum number of retries for network and replication notMaster errors (per host).
const int kMaxNumFailedHostRetryAttempts = 3;

}  // namespace

//BatchWriteExec::executeBatch中调用
AsyncRequestsSender::AsyncRequestsSender(OperationContext* opCtx,
										//轮询，Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor()
                                         executor::TaskExecutor* executor, //该AsyncRequestsSender对应的TaskExecutor
                                         const std::string db,
                                         const std::vector<AsyncRequestsSender::Request>& requests,
                                         const ReadPreferenceSetting& readPreference,
                                         Shard::RetryPolicy retryPolicy)
    : _opCtx(opCtx),
      _executor(executor),
      _db(std::move(db)),
      _readPreference(readPreference),
      _retryPolicy(retryPolicy) {
    for (const auto& request : requests) {
		//记录了请求报文，以及应该发送到那个shardId
        _remotes.emplace_back(request.shardId, request.cmdObj);
    }

    // Initialize command metadata to handle the read preference.
    _metadataObj = readPreference.toContainingBSON();

    // Schedule the requests immediately.

    // We must create the notification before scheduling any requests, because the notification is
    // signaled both on an error in scheduling the request and a request's callback.
    _notification.emplace();

    // We lock so that no callbacks signal the notification until after we are done scheduling
    // requests, to prevent signaling the notification twice, which is illegal.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _scheduleRequests(lk); //AsyncRequestsSender::_scheduleRequests
}
AsyncRequestsSender::~AsyncRequestsSender() {
    _cancelPendingRequests();

    // Wait on remaining callbacks to run.
    while (!done()) {
        next();
    }
}

//阻塞等待后端应答 BatchWriteExec::executeBatch
AsyncRequestsSender::Response AsyncRequestsSender::next() {
    invariant(!done());

    // If needed, schedule requests for all remotes which had retriable errors.
    // If some remote had success or a non-retriable error, return it.
    boost::optional<Response> readyResponse;
    while (!(readyResponse = _ready())) {
        // Otherwise, wait for some response to be received.
        if (_interruptStatus.isOK()) {
            try {
				//配合AsyncRequestsSender::_handleResponse阅读
                _notification->get(_opCtx);
            } catch (const AssertionException& ex) {
                // If the operation is interrupted, we cancel outstanding requests and switch to
                // waiting for the (canceled) callbacks to finish without checking for interrupts.
                _interruptStatus = ex.toStatus();
                _cancelPendingRequests();
                continue;
            }
        } else {
            _notification->get();
        }
    }

	//返回后端应答
    return *readyResponse;
}

void AsyncRequestsSender::stopRetrying() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stopRetrying = true;
}

//遍历_remotes，是否所有_remotes成员都done完成，配合AsyncRequestsSender::_ready()阅读
bool AsyncRequestsSender::done() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return std::all_of(
        _remotes.begin(), _remotes.end(), [](const RemoteData& remote) { return remote.done; });
}

void AsyncRequestsSender::_cancelPendingRequests() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stopRetrying = true;

    // Cancel all outstanding requests so they return immediately.
    for (auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            _executor->cancel(remote.cbHandle);
        }
    }
}

//AsyncRequestsSender::next
//获取后端Response应答构造Response返回，配合AsyncRequestsSender::_handleResponse阅读
boost::optional<AsyncRequestsSender::Response> AsyncRequestsSender::_ready() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _notification.emplace();

    if (!_stopRetrying) {
		//继续重试
        _scheduleRequests(lk);
    }

    // Check if any remote is ready.
    invariant(!_remotes.empty());
    for (auto& remote : _remotes) {
		//AsyncRequestsSender::_handleResponse中获取到了应答消息存储在swResponse中
		//取出swResponse信息，构造对应的Response
        if (remote.swResponse && !remote.done) {
			//到该后端的请求收到应答，记录done标记
            remote.done = true;
            if (remote.swResponse->isOK()) {
                invariant(remote.shardHostAndPort);
                return Response(std::move(remote.shardId),
                                std::move(remote.swResponse->getValue()),
                                std::move(*remote.shardHostAndPort));
            } else {
                // If _interruptStatus is set, promote CallbackCanceled errors to it.
                if (!_interruptStatus.isOK() &&
                    ErrorCodes::CallbackCanceled == remote.swResponse->getStatus().code()) {
                    remote.swResponse = _interruptStatus;
                }
				//如果后端应答异常，则记录异常到Response
                return Response(std::move(remote.shardId),
                                std::move(remote.swResponse->getStatus()),
                                std::move(remote.shardHostAndPort));
            }
        }
    }
    // No remotes were ready.
    return boost::none;
}

//BatchWriteExec::executeBatch->AsyncRequestsSender::AsyncRequestsSender中调用
//AsyncRequestsSender::_ready()中进行重试调用
void AsyncRequestsSender::_scheduleRequests(WithLock lk) {
    invariant(!_stopRetrying);
    // Schedule remote work on hosts for which we have not sent a request or need to retry.
    for (size_t i = 0; i < _remotes.size(); ++i) {
        auto& remote = _remotes[i];

        // First check if the remote had a retriable error, and if so, clear its response field so
        // it will be retried.
        if (remote.swResponse && !remote.done) {
            // We check both the response status and command status for a retriable error.
            Status status = remote.swResponse->getStatus();
            if (status.isOK()) {
                status = getStatusFromCommandResult(remote.swResponse->getValue().data);
            }

            if (!status.isOK()) {
                // There was an error with either the response or the command.
                auto shard = remote.getShard();
                if (!shard) {
                    remote.swResponse =
                        Status(ErrorCodes::ShardNotFound,
                               str::stream() << "Could not find shard " << remote.shardId);
                } else {
                    if (remote.shardHostAndPort) {
                        shard->updateReplSetMonitor(*remote.shardHostAndPort, status);
                    }
                    if (shard->isRetriableError(status.code(), _retryPolicy) &&
                        remote.retryCount < kMaxNumFailedHostRetryAttempts) {
                        LOG(1) << "Command to remote " << remote.shardId << " at host "
                               << *remote.shardHostAndPort
                               << " failed with retriable error and will be retried "
                               << causedBy(redact(status));
                        ++remote.retryCount;
                        remote.swResponse.reset();
                    }
                }
            }
        }

        // If the remote does not have a response or pending request, schedule remote work for it.
        if (!remote.swResponse && !remote.cbHandle.isValid()) {
			//AsyncRequestsSender::_scheduleRequest
			//发送请求到后端
            auto scheduleStatus = _scheduleRequest(lk, i); //发送走这里
            if (!scheduleStatus.isOK()) {
				//后端如果没主节点，则swResponse为对应异常
                remote.swResponse = std::move(scheduleStatus);
                // Signal the notification indicating the remote had an error (we need to do this
                // because no request was scheduled, so no callback for this remote will run and
                // signal the notification).
                if (!*_notification) {
                    _notification->set();
                }
            }
        }
    }
}

//AsyncRequestsSender::_scheduleRequests中调用
//发送请求到后端_remotes[remoteIndex]对应节点
Status AsyncRequestsSender::_scheduleRequest(WithLock, size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];

    invariant(!remote.cbHandle.isValid());
    invariant(!remote.swResponse);

	//获取分片主节点shardHostAndPort信息
    Status resolveStatus = remote.resolveShardIdToHostAndPort(_readPreference);
    if (!resolveStatus.isOK()) {
        return resolveStatus;
    }

    executor::RemoteCommandRequest request(
        *remote.shardHostAndPort, _db, remote.cmdObj, _metadataObj, _opCtx);

	//ThreadPoolTaskExecutor::scheduleRemoteCommand
    auto callbackStatus = _executor->scheduleRemoteCommand(
        request,
        stdx::bind(
        	//收到应答的回调在这里
            &AsyncRequestsSender::_handleResponse, 
            	this, stdx::placeholders::_1, remoteIndex));
    if (!callbackStatus.isOK()) {
        return callbackStatus.getStatus();
    }

    remote.cbHandle = callbackStatus.getValue();
    return Status::OK();
}

//AsyncRequestsSender::_scheduleRequest
//接收到后端应答的回调函数
void AsyncRequestsSender::_handleResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData, size_t remoteIndex) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    auto& remote = _remotes[remoteIndex];
    invariant(!remote.swResponse);

    // Clear the callback handle. This indicates that we are no longer waiting on a response from
    // 'remote'.
    remote.cbHandle = executor::TaskExecutor::CallbackHandle();

    // Store the response or error.
    if (cbData.response.status.isOK()) {
        remote.swResponse = std::move(cbData.response);
    } else {
        remote.swResponse = std::move(cbData.response.status);
    }

    // Signal the notification indicating that a remote received a response.
    //接收到后端应答信息后，置位_notification通知
    //AsyncRequestsSender::next()中接收该通知
    if (!*_notification) {
        _notification->set();
    }
}

AsyncRequestsSender::Request::Request(ShardId shardId, BSONObj cmdObj)
    : shardId(shardId), cmdObj(cmdObj) {}

AsyncRequestsSender::Response::Response(ShardId shardId,
                                        executor::RemoteCommandResponse response,
                                        HostAndPort hp)
    : shardId(std::move(shardId)),
      swResponse(std::move(response)),
      shardHostAndPort(std::move(hp)) {}

AsyncRequestsSender::Response::Response(ShardId shardId,
                                        Status status,
                                        boost::optional<HostAndPort> hp)
    : shardId(std::move(shardId)), swResponse(std::move(status)), shardHostAndPort(std::move(hp)) {}

AsyncRequestsSender::RemoteData::RemoteData(ShardId shardId, BSONObj cmdObj)
    : shardId(std::move(shardId)), cmdObj(std::move(cmdObj)) {}


//AsyncRequestsSender::_scheduleRequest中调用
//获取shardHostAndPort 
//获取后端分片主节点，如果没有主节点则走异常流程
Status AsyncRequestsSender::RemoteData::resolveShardIdToHostAndPort(
    const ReadPreferenceSetting& readPref) {
    const auto shard = getShard();
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Could not find shard " << shardId);
    }

	//RemoteCommandTargeterRS::findHostWithMaxWait
	//获取后端分片主节点，如果没有主节点则走异常流程
    auto findHostStatus = shard->getTargeter()->findHostWithMaxWait(readPref, Seconds{20});
    if (!findHostStatus.isOK()) {
        return findHostStatus.getStatus();
    }

	//如果没主节点，这里可能
    shardHostAndPort = std::move(findHostStatus.getValue());

    return Status::OK();
}

//获取shardId对应的Shard
std::shared_ptr<Shard> AsyncRequestsSender::RemoteData::getShard() {
    // TODO: Pass down an OperationContext* to use here.
    return grid.shardRegistry()->getShardNoReload(shardId);
}

}  // namespace mongo
