module Styles = {
  open Css;
  let root =
    style([display(flexBox), justifyContent(center), paddingTop(px(64))]);
  let paneSpacer =
    style([
      width(px(128)),
      media("(min-width: 1200px)", [width(px(64))]),
      media("(min-width: 1400px)", [width(px(128))]),
    ]);
};

let publishCurrentTrack = () => {
  let%Repromise result = SpotifyClient.getCurrentTrack();
  switch (result) {
  | Some((track, context, isPlaying, startTimestamp)) =>
    SpotifyStore.updateState(
      Some((track, context)),
      isPlaying ? Playing(startTimestamp) : NotPlaying,
    );
    if (isPlaying) {
      ClientSocket.publishCurrentTrack(
        UserStore.getSessionId()->Belt.Option.getExn,
        track.id,
        context.type_,
        context.id,
        startTimestamp,
      );
    };
  | None => ()
  };
  Promise.resolved();
};

[@react.component]
let make = (~roomId: string) => {
  let user = UserStore.useUser();
  let room = RoomStore.useRoom(roomId);
  let (isSyncing, setIsSyncing) = React.useState(() => false);

  if (Belt.Option.isNone(user) && isSyncing) {
    setIsSyncing(_ => false);
  };

  React.useEffect0(() => {
    ClientSocket.connectToRoom(roomId, UserStore.getSessionId());
    None;
  });

  React.useEffect1(
    () => {
      if (isSyncing) {
        SpotifyClient.turnOffRepeat() |> ignore;
      };
      None;
    },
    [|isSyncing|],
  );

  let (_, forceUpdate) = React.useState(() => 1);
  let roomPlaylist = Belt.Option.flatMap(room, room => room.playlist);
  let now = Js.Date.now();
  let roomTrackWithMetadata =
    Belt.Option.flatMap(
      roomPlaylist,
      ((tracks, startTimestamp)) => {
        let result = ref(None);
        let i = ref(0);
        let timestamp = ref(startTimestamp);
        while (Belt.Option.isNone(result^) && i^ < Js.Array.length(tracks)) {
          let track = tracks[i^];
          let durationMs = track.durationMs;
          let songEnd = timestamp^ +. durationMs;
          if (now < songEnd) {
            result := Some((track, i^, timestamp^));
          } else {
            i := i^ + 1;
            timestamp := songEnd;
          };
        };
        result^;
      },
    );
  let roomTrackId =
    Belt.Option.map(roomTrackWithMetadata, ((track, _, _)) => track.trackId);
  React.useEffect2(
    () =>
      switch (roomTrackId) {
      | Some(roomTrackId) =>
        let (roomTrack, index, startTimestamp) =
          Belt.Option.getExn(roomTrackWithMetadata);
        let positionMs = Js.Date.now() -. startTimestamp;
        // TODO
        if (isSyncing) {
          SpotifyClient.playTrack(roomTrackId, positionMs) |> ignore;
        };
        if (index < Js.Array.length(fst(Belt.Option.getExn(roomPlaylist)))
            - 1) {
          let songEnd = startTimestamp +. roomTrack.durationMs;
          let timeout =
            Js.Global.setTimeoutFloat(
              () => {forceUpdate(x => x + 1)},
              songEnd -. Js.Date.now(),
            );
          Some(() => Js.Global.clearTimeout(timeout));
        } else {
          None;
        };
      | None =>
        if (isSyncing) {
          SpotifyClient.pausePlayer() |> ignore;
        };
        None;
      },
    (roomTrackId, isSyncing),
  );

  <div className=Styles.root>
    <div>
      <RecordPlayer
        startTimestamp={Belt.Option.map(roomPlaylist, ((_, startTimestamp)) =>
          startTimestamp
        )}
        totalDuration={Belt.Option.map(
          roomPlaylist,
          ((tracks, _)) => {
            let duration = ref(0.);
            tracks
            |> Js.Array.forEach((track: SocketMessage.roomTrack) => {
                 duration := duration^ +. track.durationMs
               });
            duration^;
          },
        )}
      />
      <div> {React.string(roomId)} </div>
      {switch (user) {
       | Some(user) =>
         <div>
           <div> {React.string(user.id)} </div>
           <button onClick={_ => setIsSyncing(sync => !sync)}>
             {React.string(isSyncing ? "Stop Sync" : "Start Sync")}
           </button>
           <button
             onClick={_ => {
               if (!isSyncing) {
                 setIsSyncing(_ => true);
               };
               publishCurrentTrack() |> ignore;
             }}>
             {React.string("Publish Current Track")}
           </button>
         </div>
       | None =>
         <div> <Link path="/login"> {React.string("Login")} </Link> </div>
       }}
      {switch (room) {
       | Some(room) =>
         <div>
           <div>
             {React.string(
                string_of_int(Js.Array.length(room.connections))
                ++ " connected",
              )}
           </div>
           <div>
             {room.connections
              |> Js.Array.map((connection: SocketMessage.connection) =>
                   <div key={connection.id}>
                     {React.string(connection.userId)}
                   </div>
                 )
              |> React.array}
           </div>
         </div>
       | None => React.null
       }}
    </div>
    <div className=Styles.paneSpacer />
    <div>
      {switch (roomTrackWithMetadata) {
       | Some((roomTrack, index, startTimestamp)) =>
         <CurrentRecord
           roomPlaylist={Belt.Option.getExn(roomPlaylist)}
           roomTrack
           index
           trackStartTimestamp=startTimestamp
         />
       | None => React.null
       }}
    </div>
  </div>;
};