open Belt;
open Express;

[@bs.module]
external bodyParser: {. [@bs.meth] "json": unit => Middleware.t} =
  "body-parser";
[@bs.module] external cors: unit => Middleware.t = "cors";
let app = express();

App.disable(app, ~name="x-powered-by");
App.use(app, bodyParser##json());
App.use(app, cors());

module SocketServer = BsSocket.Server.Make(SocketMessage);

let promiseMiddleware = middleware => {
  PromiseMiddleware.from((next, req, res) =>
    Promise.Js.toBsPromise(middleware(next, req, res))
  );
};
let setProperty = (req, property, value, res) => {
  let reqData = Request.asJsonObject(req);
  Js.Dict.set(reqData, property, value);
  res;
};
let getProperty = (req, property) => {
  let reqData = Request.asJsonObject(req);
  Js.Dict.get(reqData, property);
};

App.get(app, ~path="/hello") @@
promiseMiddleware((next, req, res) =>
  res |> Response.sendString("Hello World!") |> Promise.Js.resolved
);

App.post(app, ~path="/login") @@
promiseMiddleware((next, req, res) => {
  let bodyJson = Request.bodyJSON(req) |> Option.getExn;
  let code =
    bodyJson
    ->Js.Json.decodeObject
    ->Option.getExn
    ->Js.Dict.unsafeGet("code")
    ->Js.Json.decodeString
    ->Option.getExn;
  let%Repromise (sessionId, accessToken) = Spotify.getToken(code);
  let responseJson =
    Js.Json.object_(
      Js.Dict.fromArray([|
        ("sessionId", Js.Json.string(sessionId)),
        ("accessToken", Js.Json.string(accessToken)),
      |]),
    );
  res |> Response.sendJson(responseJson) |> Promise.resolved;
});

App.getWithMany(app, ~path="/user") @@
[|
  Middleware.from((next, req, res) => {
    switch (
      Request.get("Authorization", req)->Option.flatMap(Persist.getSession)
    ) {
    | Some(session) =>
      setProperty(req, "session", Obj.magic(session), res) |> ignore;
      next(Next.middleware, res);
    | None => Response.sendStatus(Response.StatusCode.Unauthorized, res)
    }
  }),
  promiseMiddleware((next, req, res) => {
    let userId: option(string) =
      Option.map(getProperty(req, "session"), json =>
        Obj.magic(json)##userId
      );
    let responseJson =
      Js.Json.object_(
        Js.Dict.fromArray([|
          (
            "userId",
            switch (userId) {
            | Some(userId) => Js.Json.string(userId)
            | None => Js.Json.null
            },
          ),
        |]),
      );
    res |> Response.sendJson(responseJson) |> Promise.resolved;
  }),
|];

let onListen = e =>
  switch (e) {
  | exception (Js.Exn.Error(e)) =>
    Js.log(e);
    Node.Process.exit(1);
  | _ => Js.log @@ "Listening at http://127.0.0.1:3000"
  };

let server = App.listen(app, ~port=3000, ~onListen, ());

let rooms: Js.Dict.t(Room.t) = Js.Dict.empty();

let io = SocketServer.createWithHttp(server);
App.get(app, ~path="/rooms/:roomId") @@
Middleware.from((next, req, res) => {
  let roomId =
    Request.params(req)
    ->Js.Dict.unsafeGet("roomId")
    ->Js.Json.decodeString
    ->Option.getExn;
  let room = rooms->Js.Dict.get(roomId);
  switch (room) {
  | Some(room) =>
    res
    |> Response.sendJson(
         Js.Json.array(
           room.connections
           |> Js.Array.map((connection: Room.connection) =>
                Js.Json.string(connection.userId)
              ),
         ),
       )
  | None => res |> Response.sendStatus(Ok)
  };
});

SocketServer.onConnect(
  io,
  socket => {
    open SocketServer;
    Js.log("Got a connection!");
    Js.log(socket);
    let socketId = Socket.getId(socket);
    Socket.on(socket, message => {
      switch (message) {
      | JoinRoom(roomId, sessionId) =>
        Js.log2("JoinRoom", (roomId, sessionId));
        let session: User.session =
          Persist.getSession(sessionId)->Option.getExn;
        socket->Socket.join(roomId) |> ignore;
        socket
        ->Socket.to_(roomId)
        ->Socket.emit(NewUser(roomId, session.userId));
        let room =
          Option.getWithDefault(
            rooms->Js.Dict.get(roomId),
            {id: roomId, connections: [||]},
          );
        let updatedRoom =
          if (room.connections
              |> Js.Array.findIndex((connection: Room.connection) =>
                   connection.id == socketId
                 )
              == (-1)) {
            {
              ...room,
              connections:
                room.connections
                |> Js.Array.concat([|
                     (
                       {id: socketId, userId: session.userId}: Room.connection
                     ),
                   |]),
            };
          } else {
            room;
          };
        rooms->Js.Dict.set(roomId, updatedRoom);
        socket->Socket.emit(
          Connected({
            id: roomId,
            userIds:
              updatedRoom.connections
              |> Js.Array.map((connection: Room.connection) =>
                   connection.userId
                 ),
          }),
        );
      }
    });
  },
);