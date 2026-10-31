#ifndef API_H
#define API_H

// The deploy API that deployctl talks to (JSON over HTTP):
//
//   GET  /status     instances, current deployment, recent events
//   GET  /history    past deployments
//   GET  /releases   versions available in the releases folder
//   POST /deploy     {"version":"v2"}  start a canary deployment
//   POST /rollback   deploy the previous version again
//   POST /abort      stop the running deployment and roll back
//
// It only listens on 127.0.0.1, so only someone on this machine can deploy.

int  api_start(void);
void api_stop(void);

#endif
