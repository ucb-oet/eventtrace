//! Objects as a trace's events form them, by the rules eventtrace.proto states: an object is its type and ID, its
//! events are those with it as subject, and it lives from its create event to its destroy event. An event at cycle c
//! sees the state before clock edge c and its effects hold from cycle c+1, so the events of one cycle are simultaneous
//! and are handled together, as a `Cycle`.

use std::collections::HashMap;
use std::iter::Peekable;

use crate::manifest::{Lifecycle, Manifest};
use crate::trace::Event;
use crate::Result;

/// The events of one cycle, in the trace's order, which is by site and not causal.
pub struct Cycle {
    pub cycle: u64,
    pub events: Vec<Event>,
}

/// Groups a trace's events by cycle.
pub struct Cycles<I: Iterator<Item = Result<Event>>> {
    events: Peekable<I>,
}

impl<I: Iterator<Item = Result<Event>>> Cycles<I> {
    pub fn new(events: I) -> Self {
        Cycles { events: events.peekable() }
    }
}

impl<I: Iterator<Item = Result<Event>>> Iterator for Cycles<I> {
    type Item = Result<Cycle>;

    fn next(&mut self) -> Option<Result<Cycle>> {
        let first = match self.events.next()? {
            Ok(event) => event,
            Err(e) => return Some(Err(e)),
        };
        let cycle = first.cycle;
        let mut events = vec![first];
        while let Some(Ok(event)) = self.events.peek() {
            if event.cycle != cycle {
                break;
            }
            events.push(self.events.next().unwrap().unwrap());
        }
        Some(Ok(Cycle { cycle, events }))
    }
}

/// An object: its type, as the manifest names it, and its ID.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Key<'m> {
    pub object_type: &'m str,
    pub id: u64,
}

impl<'m> Key<'m> {
    /// The event's subject, if it has one.
    pub fn subject(manifest: &'m Manifest, event: &Event) -> Option<Key<'m>> {
        let object_type = manifest.events[event.event].subject.as_deref()?;
        Some(Key { object_type, id: event.subject? })
    }
}

/// The objects live after each cycle, each with its create event, which a trace holding only a window of the
/// simulation lacks for an object created before it.
pub struct Objects<'m> {
    manifest: &'m Manifest,
    live: HashMap<Key<'m>, Option<Event>>,
}

impl<'m> Objects<'m> {
    pub fn new(manifest: &'m Manifest) -> Self {
        Objects { manifest, live: HashMap::new() }
    }

    /// Applies a cycle's effects on lifetimes: a create event's subject becomes live, a destroy event's subject ends,
    /// and the subject of any other event, not yet seen, was created before the trace began. A destroy wins over the
    /// cycle's other events, which are simultaneous with it.
    pub fn commit(&mut self, cycle: &Cycle) {
        let mut destroyed = Vec::new();
        for event in &cycle.events {
            let Some(key) = Key::subject(self.manifest, event) else { continue };
            match self.manifest.events[event.event].lifecycle {
                Some(Lifecycle::Create) => {
                    self.live.insert(key, Some(event.clone()));
                }
                Some(Lifecycle::Destroy) => destroyed.push(key),
                None => {
                    self.live.entry(key).or_insert(None);
                }
            }
        }
        for key in destroyed {
            self.live.remove(&key);
        }
    }

    pub fn is_live(&self, key: &Key<'m>) -> bool {
        self.live.contains_key(key)
    }

    /// A live object's create event, if the trace has it.
    pub fn create_event(&self, key: &Key<'m>) -> Option<&Event> {
        self.live.get(key)?.as_ref()
    }
}
