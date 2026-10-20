//! The manifest, which the frontend writes at elaboration: each event with its subject, lifecycle, and fields, each
//! object type with its fields and their properties, each enum, and each site, marking those that find their subject
//! blocked. A field naming the object one came from, aborted it, or blocks it has a role. A manifest older than object
//! types lists none.

use std::collections::HashMap;
use std::path::Path;

use indexmap::IndexMap;
use serde::Deserialize;
use sha2::{Digest, Sha256};

use crate::{Error, Result};

#[derive(Debug, Deserialize)]
pub struct Manifest {
    pub events: Vec<EventSchema>,
    #[serde(default)]
    pub objects: Vec<ObjectSchema>,
    pub enums: Vec<EnumSchema>,
    pub sites: Vec<Site>,

    /// SHA-256 of the manifest's bytes, in hex, which a trace's header must carry.
    #[serde(skip)]
    pub sha256: String,
    /// Each site's event, as an index into `events`, by site ID.
    #[serde(skip)]
    site_events: Vec<Option<usize>>,
    /// Each enum's variant names by value.
    #[serde(skip)]
    variants: HashMap<String, HashMap<u64, String>>,
}

#[derive(Debug, Deserialize)]
pub struct EventSchema {
    pub name: String,
    /// The object type of the event's subject, if it has one.
    pub subject: Option<String>,
    pub lifecycle: Option<Lifecycle>,
    pub fields: Vec<FieldSchema>,
}

impl EventSchema {
    /// The event's name without its object type, as `stall` for `instruction.stall`.
    pub fn short_name(&self) -> &str {
        self.name.split_once('.').map_or(&self.name, |(_, short)| short)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Lifecycle {
    Create,
    /// Ends the subject normally, having done its work.
    Complete,
    /// Ends the subject abnormally, before it finished its work.
    Abort,
}

impl Lifecycle {
    /// Whether the event ends its subject's life.
    pub fn ends(self) -> bool {
        matches!(self, Lifecycle::Complete | Lifecycle::Abort)
    }
}

/// What a field naming another object means, beyond its value.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Role {
    /// An object field: the object this one came from.
    Parent,
    /// The object that aborted the subject.
    AbortedBy,
    /// The object the subject is blocked by.
    BlockedBy,
}

#[derive(Debug, Deserialize)]
pub struct FieldSchema {
    pub name: String,
    #[serde(rename = "type")]
    pub ty: FieldType,
    /// Bits, for every type but Id.
    pub width: Option<u32>,
    /// An Enum field's enum.
    #[serde(rename = "enum")]
    pub enum_name: Option<String>,
    /// An Id field's object type.
    pub object: Option<String>,
    /// How often an object type's field may be assigned; an event's fields have none.
    pub property: Option<Property>,
    pub role: Option<Role>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Property {
    /// Set by the create event and never again.
    Const,
    SetOnce,
    Mutable,
}

#[derive(Debug, Deserialize)]
pub struct ObjectSchema {
    pub name: String,
    /// In declaration order.
    pub fields: Vec<FieldSchema>,
}

impl ObjectSchema {
    /// The field naming the object this one came from, if the type has one.
    pub fn parent(&self) -> Option<&FieldSchema> {
        self.fields.iter().find(|f| f.role == Some(Role::Parent))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
pub enum FieldType {
    Bool,
    UInt,
    SInt,
    Enum,
    Id,
}

#[derive(Debug, Deserialize)]
pub struct EnumSchema {
    pub name: String,
    /// Each variant's value, in declaration order.
    pub values: IndexMap<String, u64>,
}

#[derive(Debug, Deserialize)]
pub struct Site {
    pub site_id: u32,
    pub event_name: String,
    pub instance: String,
    pub source: String,
    /// Whether the site's records find their subject blocked.
    #[serde(default)]
    pub blocks: bool,
}

impl Manifest {
    pub fn load(path: &Path) -> Result<Manifest> {
        let bytes = std::fs::read(path).map_err(|e| Error(format!("cannot read {}: {e}", path.display())))?;
        let mut manifest: Manifest = serde_json::from_slice(&bytes)
            .map_err(|e| Error(format!("{} is not a manifest: {e}", path.display())))?;
        manifest.sha256 = Sha256::digest(&bytes).iter().map(|b| format!("{b:02x}")).collect();

        let by_name: HashMap<&str, usize> = manifest.events.iter().enumerate().map(|(i, e)| (e.name.as_str(), i)).collect();
        let mut site_events = Vec::new();
        for site in &manifest.sites {
            let event = *by_name.get(site.event_name.as_str()).ok_or_else(|| {
                Error(format!("{}: site {} emits {}, which it does not list", path.display(), site.site_id, site.event_name))
            })?;
            let id = site.site_id as usize;
            if site_events.len() <= id {
                site_events.resize(id + 1, None);
            }
            site_events[id] = Some(event);
        }
        manifest.site_events = site_events;
        manifest.variants = manifest
            .enums
            .iter()
            .map(|e| (e.name.clone(), e.values.iter().map(|(name, &value)| (value, name.clone())).collect()))
            .collect();
        Ok(manifest)
    }

    /// The event a site emits, as an index into `events`.
    pub fn site_event(&self, site_id: u32) -> Option<usize> {
        self.site_events.get(site_id as usize).copied().flatten()
    }

    /// The name of an enum's variant with this value.
    pub fn variant(&self, enum_name: &str, value: u64) -> Option<&str> {
        self.variants.get(enum_name)?.get(&value).map(String::as_str)
    }

    /// An object type, if the manifest lists object types.
    pub fn object(&self, name: &str) -> Option<&ObjectSchema> {
        self.objects.iter().find(|o| o.name == name)
    }

    /// The position of an event's field, in the manifest's order.
    pub fn field_index(&self, event: usize, field: &str) -> Option<usize> {
        self.events[event].fields.iter().position(|f| f.name == field)
    }
}
